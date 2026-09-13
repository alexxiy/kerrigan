// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ACTIVE_DKGSESSIONHANDLER_H
#define BITCOIN_ACTIVE_DKGSESSIONHANDLER_H

#include <llmq/dkgsessionhandler.h>
#include <llmq/options.h>

#include <gsl/pointers.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class CActiveMasternodeManager;
class CBLSWorker;
class CBlockIndex;
class CConnman;
class ChainstateManager;
class CDeterministicMNManager;
class CMasternodeMetaMan;
class CSporkManager;
class PeerManager;
namespace Consensus {
struct LLMQParams;
} // namespace Consensus
namespace llmq {
class CDKGSession;
class CDKGDebugManager;
class CDKGSessionManager;
class CQuorumBlockProcessor;
class CQuorumSnapshotManager;
} // namespace llmq

namespace llmq {
class ActiveDKGSessionHandler final : public llmq::CDKGSessionHandler
{
    using StartPhaseFunc = std::function<void()>;
    using WhileWaitFunc = std::function<bool()>;

private:
    CBLSWorker& m_bls_worker;
    CDeterministicMNManager& m_dmnman;
    CMasternodeMetaMan& m_mn_metaman;
    llmq::CDKGDebugManager& m_dkgdbgman;
    llmq::CDKGSessionManager& m_qdkgsman;
    llmq::CQuorumBlockProcessor& m_qblockman;
    llmq::CQuorumSnapshotManager& m_qsnapman;
    const CActiveMasternodeManager& m_mn_activeman;
    const ChainstateManager& m_chainman;
    const CSporkManager& m_sporkman;
    const bool m_quorums_watch{false};
    //! -llmqdkgphasedwell, minimum seconds of wall clock between two phase start handlers
    const int m_dkg_phase_dwell{0};
    //! -llmqdkgphasedwellmode
    const DkgPhaseDwellMode m_dkg_phase_dwell_mode{DkgPhaseDwellMode::Auto};
    const int quorumIndex;

private:
    std::atomic<bool> stopRequested{false};
    std::atomic<int> currentHeight{-1};
    std::string m_thread_name;
    std::thread phaseHandlerThread;
    std::unique_ptr<CDKGSession> curSession{nullptr};

    //! Wall clock (ms since epoch) at which the last phase start handler of the current round
    //! ran, 0 when no start handler ran yet for this round.
    std::atomic<int64_t> m_phase_start_ms{0};
    //! Whether the dwell applies to the round currently being handled, decided once per round.
    std::atomic<bool> m_dwell_armed{false};

    mutable Mutex cs_phase_qhash;
    QuorumPhase phase GUARDED_BY(cs_phase_qhash){QuorumPhase::Idle};
    uint256 quorumHash GUARDED_BY(cs_phase_qhash);

public:
    ActiveDKGSessionHandler() = delete;
    ActiveDKGSessionHandler(const ActiveDKGSessionHandler&) = delete;
    ActiveDKGSessionHandler& operator=(const ActiveDKGSessionHandler&) = delete;
    ActiveDKGSessionHandler(CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
                            llmq::CDKGDebugManager& dkgdbgman, llmq::CDKGSessionManager& qdkgsman,
                            llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                            const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman,
                            const CSporkManager& sporkman, const Consensus::LLMQParams& llmq_params, bool quorums_watch,
                            int dkg_phase_dwell, DkgPhaseDwellMode dkg_phase_dwell_mode, int quorums_idx);
    ~ActiveDKGSessionHandler();

public:
    //! CDKGSessionHandler
    bool GetContribution(const uint256& hash, CDKGContribution& ret) const override;
    bool GetComplaint(const uint256& hash, CDKGComplaint& ret) const override;
    bool GetJustification(const uint256& hash, CDKGJustification& ret) const override;
    bool GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const override;
    QuorumPhase GetPhase() const override EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void StartThread(CConnman& connman, PeerManager& peerman) override;
    void StopThread() override;
    void UpdatedBlockTip(const CBlockIndex* pindexNew) override EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);

private:
    std::pair<QuorumPhase, uint256> GetPhaseAndQuorumHash() const EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);

    bool InitNewQuorum(gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex);

    /**
     * @param curPhase current QuorumPhase
     * @param nextPhase next QuorumPhase
     * @param expectedQuorumHash expected QuorumHash, defaults to null
     * @param shouldNotWait function that returns bool, defaults to function that returns false. If the function returns false, we will wait in the loop, if true, we don't wait
     */
    void WaitForNextPhase(
        std::optional<QuorumPhase> curPhase, QuorumPhase nextPhase, const uint256& expectedQuorumHash = uint256(),
        const WhileWaitFunc& shouldNotWait = [] { return false; }) const EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void WaitForNewQuorum(const uint256& oldQuorumHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void SleepBeforePhase(QuorumPhase curPhase, const uint256& expectedQuorumHash, double randomSleepFactor,
                          const WhileWaitFunc& runWhileWaiting) const EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    bool ComputeDwellArmed(const CConnman& connman) const;
    void WaitPhaseDwell(QuorumPhase curPhase, const uint256& expectedQuorumHash,
                        const WhileWaitFunc& runWhileWaiting) const EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    template <typename Message>
    void DrainPendingMessages(const CConnman& connman, CDKGPendingMessages& pendingMessages, PeerManager& peerman,
                              const uint256& expectedQuorumHash) EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase, const uint256& expectedQuorumHash,
                     double randomSleepFactor, const StartPhaseFunc& startPhaseFunc,
                     const WhileWaitFunc& runWhileWaiting) EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void HandleDKGRound(CConnman& connman, PeerManager& peerman) EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
    void PhaseHandlerThread(CConnman& connman, PeerManager& peerman) EXCLUSIVE_LOCKS_REQUIRED(!cs_phase_qhash);
};
} // namespace llmq

#endif // BITCOIN_ACTIVE_DKGSESSIONHANDLER_H
