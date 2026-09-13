// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/context.h>

#include <active/dkgsessionhandler.h>
#include <active/masternode.h>
#include <active/quorums.h>
#include <chainlock/handler.h>
#include <chainlock/signing.h>
#include <governance/governance.h>
#include <governance/signing.h>
#include <instantsend/instantsend.h>
#include <instantsend/signing.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/ehf_signals.h>
#include <llmq/quorumsman.h>
#include <llmq/signing_shares.h>
#include <recovery/recovery.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

ActiveContext::ActiveContext(CBLSWorker& bls_worker, ChainstateManager& chainman, CConnman& connman,
                             CDeterministicMNManager& dmnman, CGovernanceManager& govman, CMasternodeMetaMan& mn_metaman,
                             CSporkManager& sporkman, const chainlock::Chainlocks& chainlocks, CTxMemPool& mempool,
                             chainlock::ChainlockHandler& clhandler, llmq::CInstantSendManager& isman,
                             llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                             llmq::CQuorumSnapshotManager& qsnapman, llmq::CSigningManager& sigman,
                             const CMasternodeSync& mn_sync, const CBLSSecretKey& operator_sk,
                             const llmq::QvvecSyncModeMap& sync_map, const util::DbWrapperParams& db_params,
                             bool quorums_recovery, bool quorums_watch, int dkg_phase_dwell,
                             llmq::DkgPhaseDwellMode dkg_phase_dwell_mode, bool selfcheck_strict) :
    m_isman{isman},
    m_qman{qman},
    m_dmnman{dmnman},
    m_qblockman{qblockman},
    m_qsnapman{qsnapman},
    m_chainman{chainman},
    nodeman{std::make_unique<CActiveMasternodeManager>(connman, dmnman, operator_sk, selfcheck_strict)},
    dkgdbgman{std::make_unique<llmq::CDKGDebugManager>()},
    qdkgsman{std::make_unique<llmq::CDKGSessionManager>(dmnman, qsnapman, chainman, sporkman, db_params, quorums_watch)},
    shareman{std::make_unique<llmq::CSigSharesManager>(connman, chainman, sigman, *nodeman, qman, sporkman)},
    gov_signer{std::make_unique<GovernanceSigner>(connman, dmnman, govman, *nodeman, chainman, mn_sync)},
    ehf_sighandler{std::make_unique<llmq::CEHFSignalsHandler>(chainman, sigman, *shareman, qman)},
    qman_handler{std::make_unique<llmq::QuorumParticipant>(bls_worker, connman, dmnman, qman, qsnapman, *nodeman, chainman,
                                                           mn_sync, sporkman, sync_map, quorums_recovery, quorums_watch)},
    cl_signer{std::make_unique<chainlock::ChainLockSigner>(chainman.ActiveChainstate(), chainlocks, clhandler, isman,
                                                           qman, sigman, *shareman, mn_sync)},
    is_signer{std::make_unique<instantsend::InstantSendSigner>(chainman.ActiveChainstate(), chainlocks, isman, sigman,
                                                               *shareman, qman, sporkman, mempool, mn_sync)}
{
    qdkgsman->InitializeHandlers([&](const Consensus::LLMQParams& llmq_params,
                                     int quorum_idx) -> std::unique_ptr<llmq::ActiveDKGSessionHandler> {
        return std::make_unique<llmq::ActiveDKGSessionHandler>(bls_worker, dmnman, mn_metaman, *dkgdbgman, *qdkgsman,
                                                               qblockman, qsnapman, *nodeman, chainman, sporkman,
                                                               llmq_params, quorums_watch, dkg_phase_dwell,
                                                               dkg_phase_dwell_mode, quorum_idx);
    });
    m_isman.ConnectSigner(is_signer.get());
    m_qman.ConnectManagers(qman_handler.get(), qdkgsman.get());
}

ActiveContext::~ActiveContext()
{
    m_qman.DisconnectManagers();
    m_isman.DisconnectSigner();
}

void ActiveContext::Start(CConnman& connman, PeerManager& peerman)
{
    qman_handler->Start();
    qdkgsman->StartThreads(connman, peerman);
    cl_signer->Start();
    cl_signer->RegisterRecoveryInterface();
    is_signer->RegisterRecoveryInterface();

    RegisterValidationInterface(cl_signer.get());
}

void ActiveContext::Stop()
{
    UnregisterValidationInterface(cl_signer.get());

    is_signer->UnregisterRecoveryInterface();
    cl_signer->UnregisterRecoveryInterface();
    cl_signer->Stop();
    qdkgsman->StopThreads();
    qman_handler->Stop();
}

void ActiveContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    // Quarantine gate (WS-HEAL v2 5.2.3): a quarantined masternode performs
    // no duties -- no MN state updates, EHF/governance signing, DKG ticks or
    // quorum handling -- even if networking were somehow re-enabled.
    if (recovery::IsQuarantined()) return;

    nodeman->UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload);
    UpdateDkgOutcomes(pindexNew);
    ehf_sighandler->UpdatedBlockTip(pindexNew);
    gov_signer->UpdatedBlockTip(pindexNew);
    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
    qman_handler->UpdatedBlockTip(pindexNew, fInitialDownload);
}

void ActiveContext::UpdateDkgOutcomes(const CBlockIndex* pindexNew)
{
    const uint256 proTxHash{nodeman->GetProTxHash()};
    if (proTxHash.IsNull()) return;

    // A commitment can only be mined inside a type's mining window. Once the cache has been primed
    // every other tip, including every extra tip of a burst, costs a single height comparison.
    if (WITH_LOCK(m_dkg_outcomes_mutex, return m_dkg_outcomes_primed) &&
        !llmq::IsPossibleCommitmentHeight(pindexNew->nHeight)) {
        return;
    }

    const auto outcomes{llmq::GetRecentDkgOutcomes(proTxHash, m_qblockman, m_dmnman, m_qsnapman, m_chainman,
                                                   pindexNew)};

    LOCK(m_dkg_outcomes_mutex);
    // The first scan only fills the cache: those rounds ended before we were watching, and
    // announcing them at startup would read as fresh news.
    const bool priming{!m_dkg_outcomes_primed};
    m_dkg_outcomes_primed = true;

    for (const auto& outcome : outcomes) {
        auto& cached = m_dkg_outcomes[{outcome.llmqType, outcome.quorumIndex}];
        // Same commitment as last time, or an older one because a reorg moved us back: say nothing.
        if (cached.minedBlockHash == outcome.minedBlockHash || outcome.height <= cached.height) continue;
        cached = outcome;
        if (priming) continue;

        // Operators currently learn about an exclusion only when the PoSe ban lands, which is too
        // late to correct anything, so say it out loud at the default log level.
        LogPrintf("DKG: masternode %s was %s in quorum %s %s qi[%d] mined at height %d%s\n", proTxHash.ToString(),
                  outcome.valid ? "valid" : "EXCLUDED", outcome.llmqTypeName, outcome.quorumHash.ToString(),
                  outcome.quorumIndex, outcome.height,
                  outcome.valid ? "" : " (PoSe penalty applied by the network)");
    }
}

std::optional<llmq::DkgOutcome> ActiveContext::GetLastDkgOutcome() const
{
    LOCK(m_dkg_outcomes_mutex);
    const llmq::DkgOutcome* newest{nullptr};
    for (const auto& [key, outcome] : m_dkg_outcomes) {
        if (outcome.height < 0) continue;
        if (newest == nullptr || outcome.height > newest->height) {
            newest = &outcome;
        }
    }
    if (newest == nullptr) return std::nullopt;
    return *newest;
}
