// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BGL_NET_PROCESSING_H
#define BGL_NET_PROCESSING_H

#include <consensus/params.h>
#include <net.h>
#include <sync.h>
#include <txrequest.h>
#include <validationinterface.h>

class BlockTransactionsRequest;
class BlockValidationState;
class CBlockHeader;
class CChainParams;
class CTxMemPool;
class ChainstateManager;
class TxValidationState;

extern RecursiveMutex cs_main;
extern RecursiveMutex g_cs_orphans;

/** Default for -maxorphantx, maximum number of orphan transactions kept in memory */
static const unsigned int DEFAULT_MAX_ORPHAN_TRANSACTIONS = 100;
/** Default number of orphan+recently-replaced txn to keep around for block reconstruction */
static const unsigned int DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN = 100;
static const bool DEFAULT_PEERBLOOMFILTERS = false;
static const bool DEFAULT_PEERBLOCKFILTERS = false;
/** Threshold for marking a node to be discouraged, e.g. disconnected and added to the discouragement filter. */
static const int DISCOURAGEMENT_THRESHOLD{100};

struct CNodeStateStats {
    int nSyncHeight = -1;
    int nCommonHeight = -1;
    int m_starting_height = -1;
    std::vector<int> vHeightInFlight;
};

class PeerManager : public CValidationInterface, public NetEventsInterface
{
public:
    static std::unique_ptr<PeerManager> make(const CChainParams& chainparams, CConnman& connman, BanMan* banman,
                                             CScheduler& scheduler, ChainstateManager& chainman, CTxMemPool& pool,
                                             bool ignore_incoming_txs);
    virtual ~PeerManager() { }

    /** Get statistics from node state */
    virtual bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) = 0;

    /** Whether this node ignores txs received over p2p. */
    virtual bool IgnoresIncomingTxs() = 0;

    /** Set the best height */
    virtual void SetBestHeight(int height) = 0;

    /**
     * Increment peer's misbehavior score. If the new value >= DISCOURAGEMENT_THRESHOLD, mark the node
     * to be discouraged, meaning the peer might be disconnected and added to the discouragement filter.
     * Public for unit testing.
     */
    virtual void Misbehaving(const NodeId pnode, const int howmuch, const std::string& message) = 0;

    /**
     * Evict extra outbound peers. If we think our tip may be stale, connect to an extra outbound.
     * Public for unit testing.
     */
    virtual void CheckForStaleTipAndEvictPeers() = 0;

    /** Process a single message from a peer. Public for fuzz testing */
    virtual void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                                const std::chrono::microseconds time_received, const std::atomic<bool>& interruptMsgProc) = 0;
};

class PeerManagerImpl final : public PeerManager
{
public:
    PeerManagerImpl(const CChainParams& chainparams, CConnman& connman, BanMan* banman,
                    CScheduler& scheduler, ChainstateManager& chainman, CTxMemPool& pool,
                    bool ignore_incoming_txs);

    /** Overridden from CValidationInterface. */
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexConnected) override;
    void BlockDisconnected(const std::shared_ptr<const CBlock> &block, const CBlockIndex* pindex) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override;
    void NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock>& pblock) override;

    /** Implement NetEventsInterface */
    void InitializeNode(CNode* pnode) override;
    void FinalizeNode(const CNode& node, bool& fUpdateConnectionTime) override;
    bool ProcessMessages(CNode* pfrom, std::atomic<bool>& interrupt) override;
    bool SendMessages(CNode* pto) override EXCLUSIVE_LOCKS_REQUIRED(pto->cs_sendProcessing);

    /** Implement PeerManager */
    void CheckForStaleTipAndEvictPeers() override;
    bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) override;
    bool IgnoresIncomingTxs() override { return m_ignore_incoming_txs; }
    void SetBestHeight(int height) override { m_best_height = height; };
    void Misbehaving(const NodeId pnode, const int howmuch, const std::string& message) override;
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                        const std::chrono::microseconds time_received, const std::atomic<bool>& interruptMsgProc) override;

private:
    /** Consider evicting an outbound peer based on the amount of time they've been behind our tip */
    void ConsiderEviction(CNode& pto, int64_t time_in_seconds) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** If we have extra outbound peers, try to disconnect the one with the oldest block announcement */
    void EvictExtraOutboundPeers(int64_t time_in_seconds) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Retrieve unbroadcast transactions from the mempool and reattempt sending to peers */
    void ReattemptInitialBroadcast(CScheduler& scheduler) const;

    /** Get a shared pointer to the Peer object.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef GetPeerRef(NodeId id) const;

    /** Get a shared pointer to the Peer object and remove it from m_peer_map.
     *  May return an empty shared_ptr if the Peer object can't be found. */
    PeerRef RemovePeer(NodeId id);

    /**
     * Potentially mark a node discouraged based on the contents of a BlockValidationState object
     *
     * @param[in] via_compact_block this bool is passed in because net_processing should
     * punish peers differently depending on whether the data was provided in a compact
     * block message or not. If the compact block had a valid header, but contained invalid
     * txs, the peer should not be punished. See BIP 152.
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForBlock(NodeId nodeid, const BlockValidationState& state,
                                 bool via_compact_block, const std::string& message = "");

    /**
     * Potentially disconnect and discourage a node based on the contents of a TxValidationState object
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForTx(NodeId nodeid, const TxValidationState& state, const std::string& message = "");

    /** Maybe disconnect a peer and discourage future connections from its address.
     *
     * @param[in]   pnode     The node to check.
     * @return                True if the peer was marked for disconnection in this function
     */
    bool MaybeDiscourageAndDisconnect(CNode& pnode);

    void ProcessOrphanTx(std::set<uint256>& orphan_work_set) EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_cs_orphans);
    /** Process a single headers message from a peer. */
    void ProcessHeadersMessage(CNode& pfrom, const Peer& peer,
                               const std::vector<CBlockHeader>& headers,
                               bool via_compact_block);

    void SendBlockTransactions(CNode& pfrom, const CBlock& block, const BlockTransactionsRequest& req);

    /** Register with TxRequestTracker that an INV has been received from a
     *  peer. The announcement parameters are decided in PeerManager and then
     *  passed to TxRequestTracker. */
    void AddTxAnnouncement(const CNode& node, const GenTxid& gtxid, std::chrono::microseconds current_time)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Send a version message to a peer */
    void PushNodeVersion(CNode& pnode, int64_t nTime);

    const CChainParams& m_chainparams;
    CConnman& m_connman;
    /** Pointer to this node's banman. May be nullptr - check existence before dereferencing. */
    BanMan* const m_banman;
    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;
    TxRequestTracker m_txrequest GUARDED_BY(::cs_main);

    /** The height of the best chain */
    std::atomic<int> m_best_height{-1};

    int64_t m_stale_tip_check_time; //!< Next time to check for stale tip

    /** Whether this node is running in blocks only mode */
    const bool m_ignore_incoming_txs;

    /** Whether we've completed initial sync yet, for determining when to turn
      * on extra block-relay-only peers. */
    bool m_initial_sync_finished{false};

    /** Protects m_peer_map. This mutex must not be locked while holding a lock
     *  on any of the mutexes inside a Peer object. */
    mutable Mutex m_peer_mutex;
    /**
     * Map of all Peer objects, keyed by peer id. This map is protected
     * by the m_peer_mutex. Once a shared pointer reference is
     * taken, the lock may be released. Individual fields are protected by
     * their own locks.
     */
    std::map<NodeId, PeerRef> m_peer_map GUARDED_BY(m_peer_mutex);
};

/** Relay transaction to every node */
void RelayTransaction(const uint256& txid, const uint256& wtxid, const CConnman& connman) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

#endif // BGL_NET_PROCESSING_H
