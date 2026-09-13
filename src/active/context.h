// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ACTIVE_CONTEXT_H
#define BITCOIN_ACTIVE_CONTEXT_H

#include <active/quorums.h>
#include <llmq/options.h>
#include <llmq/types.h>

#include <sync.h>
#include <uint256.h>
#include <validationinterface.h>

#include <map>
#include <memory>
#include <optional>
#include <utility>

class CActiveMasternodeManager;
class CBLSSecretKey;
class CBLSWorker;
class ChainstateManager;
class CConnman;
class CDeterministicMNManager;
class CGovernanceManager;
class CMasternodeMetaMan;
class CMasternodeSync;
class CScheduler;
class CMNHFManager;
class CSporkManager;
class CTxMemPool;
class GovernanceSigner;
class PeerManager;
namespace chainlock {
class Chainlocks;
class ChainlockHandler;
class ChainLockSigner;
} // namespace chainlock
namespace instantsend {
class InstantSendSigner;
} // namespace instantsend
namespace llmq {
class CDKGDebugManager;
class CDKGSessionManager;
class CEHFSignalsHandler;
class CInstantSendManager;
class CQuorumBlockProcessor;
class CQuorumManager;
class CQuorumSnapshotManager;
class CSigningManager;
class CSigSharesManager;
class QuorumParticipant;
} // namespace llmq
namespace util {
struct DbWrapperParams;
} // namespace util

struct ActiveContext final : public CValidationInterface {
private:
    llmq::CInstantSendManager& m_isman;
    llmq::CQuorumManager& m_qman;
    CDeterministicMNManager& m_dmnman;
    llmq::CQuorumBlockProcessor& m_qblockman;
    llmq::CQuorumSnapshotManager& m_qsnapman;
    const ChainstateManager& m_chainman;
    // Newest mined commitment seen per (LLMQ type, quorum index). Filled in UpdatedBlockTip so the
    // RPC never has to rescan, and used to suppress repeats within a burst or after a reorg.
    mutable Mutex m_dkg_outcomes_mutex;
    std::map<std::pair<Consensus::LLMQType, int>, llmq::DkgOutcome> m_dkg_outcomes GUARDED_BY(m_dkg_outcomes_mutex);
    bool m_dkg_outcomes_primed GUARDED_BY(m_dkg_outcomes_mutex){false};
    // Highest tip we have already considered, so a burst is judged on the span it connected and
    // not only on the height it happened to stop at.
    int m_last_scanned_height GUARDED_BY(m_dkg_outcomes_mutex){-1};

public:
    ActiveContext() = delete;
    ActiveContext(const ActiveContext&) = delete;
    ActiveContext& operator=(const ActiveContext&) = delete;
    explicit ActiveContext(CBLSWorker& bls_worker, ChainstateManager& chainman, CConnman& connman,
                           CDeterministicMNManager& dmnman, CGovernanceManager& govman, CMasternodeMetaMan& mn_metaman,
                           CSporkManager& sporkman, const chainlock::Chainlocks& chainlocks, CTxMemPool& mempool,
                           chainlock::ChainlockHandler& clhandler, llmq::CInstantSendManager& isman,
                           llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                           llmq::CQuorumSnapshotManager& qsnapman, llmq::CSigningManager& sigman,
                           const CMasternodeSync& mn_sync, const CBLSSecretKey& operator_sk,
                           const llmq::QvvecSyncModeMap& sync_map, const util::DbWrapperParams& db_params,
                           bool quorums_recovery, bool quorums_watch, int dkg_phase_dwell,
                           llmq::DkgPhaseDwellMode dkg_phase_dwell_mode, bool selfcheck_strict);
    ~ActiveContext();

    void Start(CConnman& connman, PeerManager& peerman, CScheduler& scheduler);
    void Stop();

    //! Newest mined commitment this masternode was a member of, or nullopt when there is none.
    [[nodiscard]] std::optional<llmq::DkgOutcome> GetLastDkgOutcome() const EXCLUSIVE_LOCKS_REQUIRED(!m_dkg_outcomes_mutex);

protected:
    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

private:
    // Tell the operator how the DKG rounds this masternode took part in actually ended.
    void UpdateDkgOutcomes(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                           const uint256& proTxHash) EXCLUSIVE_LOCKS_REQUIRED(!m_dkg_outcomes_mutex);

public:
    /*
     * Entities that are only utilized when masternode mode is enabled
     * and are accessible in their own right
     */
    const std::unique_ptr<CActiveMasternodeManager> nodeman;
    const std::unique_ptr<llmq::CDKGDebugManager> dkgdbgman;
    const std::unique_ptr<llmq::CDKGSessionManager> qdkgsman;
    const std::unique_ptr<llmq::CSigSharesManager> shareman;

private:
    const std::unique_ptr<GovernanceSigner> gov_signer;
    const std::unique_ptr<llmq::CEHFSignalsHandler> ehf_sighandler;
    const std::unique_ptr<llmq::QuorumParticipant> qman_handler;
    const std::unique_ptr<chainlock::ChainLockSigner> cl_signer;
    const std::unique_ptr<instantsend::InstantSendSigner> is_signer;
};

#endif // BITCOIN_ACTIVE_CONTEXT_H
