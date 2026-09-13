// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/dkgsessionhandler.h>

#include <active/dkgsession.h>
#include <active/masternode.h>
#include <evo/deterministicmns.h>
#include <llmq/blockprocessor.h>
#include <llmq/debug.h>
#include <llmq/dkgsession.h>
#include <llmq/options.h>
#include <llmq/utils.h>

#include <deploymentstatus.h>
#include <logging.h>
#include <recovery/recovery.h>
#include <saltedhasher.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>

namespace llmq {
ActiveDKGSessionHandler::ActiveDKGSessionHandler(
    CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
    llmq::CDKGDebugManager& dkgdbgman, llmq::CDKGSessionManager& qdkgsman, llmq::CQuorumBlockProcessor& qblockman,
    llmq::CQuorumSnapshotManager& qsnapman, const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman,
    const CSporkManager& sporkman, const Consensus::LLMQParams& llmq_params, bool quorums_watch, int dkg_phase_dwell,
    DkgPhaseDwellMode dkg_phase_dwell_mode, int quorums_idx) :
    llmq::CDKGSessionHandler(llmq_params),
    m_bls_worker{bls_worker},
    m_dmnman{dmnman},
    m_mn_metaman{mn_metaman},
    m_dkgdbgman{dkgdbgman},
    m_qdkgsman{qdkgsman},
    m_qblockman{qblockman},
    m_qsnapman{qsnapman},
    m_mn_activeman{mn_activeman},
    m_chainman{chainman},
    m_sporkman{sporkman},
    m_quorums_watch{quorums_watch},
    m_dkg_phase_dwell{dkg_phase_dwell},
    m_dkg_phase_dwell_mode{dkg_phase_dwell_mode},
    quorumIndex{quorums_idx}
{
}

ActiveDKGSessionHandler::~ActiveDKGSessionHandler() = default;

void ActiveDKGSessionHandler::UpdatedBlockTip(const CBlockIndex* pindexNew)
{
    //AssertLockNotHeld(cs_main);
    //Indexed quorums (greater than 0) are enabled with Quorum Rotation
    if (quorumIndex > 0 && !IsQuorumRotationEnabled(params, pindexNew)) {
        return;
    }
    LOCK(cs_phase_qhash);

    int quorumStageInt = (pindexNew->nHeight - quorumIndex) % params.dkgInterval;

    const CBlockIndex* pQuorumBaseBlockIndex = pindexNew->GetAncestor(pindexNew->nHeight - quorumStageInt);

    currentHeight = pindexNew->nHeight;
    quorumHash = pQuorumBaseBlockIndex->GetBlockHash();

    bool fNewPhase = (quorumStageInt % params.dkgPhaseBlocks) == 0;
    int phaseInt = quorumStageInt / params.dkgPhaseBlocks + 1;
    QuorumPhase oldPhase = phase;
    if (fNewPhase && phaseInt >= ToUnderlying(QuorumPhase::Initialized) && phaseInt <= ToUnderlying(QuorumPhase::Idle)) {
        phase = static_cast<QuorumPhase>(phaseInt);
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] currentHeight=%d, pQuorumBaseBlockIndex->nHeight=%d, oldPhase=%d, newPhase=%d\n", __func__,
             params.name, quorumIndex, currentHeight, pQuorumBaseBlockIndex->nHeight, ToUnderlying(oldPhase), ToUnderlying(phase));
}

void ActiveDKGSessionHandler::StartThread(CConnman& connman, PeerManager& peerman)
{
    if (phaseHandlerThread.joinable()) {
        throw std::runtime_error("Tried to start an already started ActiveDKGSessionHandler thread.");
    }

    m_thread_name = strprintf("llmq-%d-%d", ToUnderlying(params.type), quorumIndex);
    phaseHandlerThread = std::thread(&util::TraceThread, m_thread_name.c_str(),
                                     [this, &connman, &peerman] { PhaseHandlerThread(connman, peerman); });
}

void ActiveDKGSessionHandler::StopThread()
{
    stopRequested = true;
    if (phaseHandlerThread.joinable()) {
        phaseHandlerThread.join();
    }
}

std::pair<QuorumPhase, uint256> ActiveDKGSessionHandler::GetPhaseAndQuorumHash() const
{
    LOCK(cs_phase_qhash);
    return std::make_pair(phase, quorumHash);
}

namespace {
//! One summary line per LLMQ type per day is enough to tell an operator the type cannot form.
constexpr int64_t UNFORMABLE_QUORUM_LOG_INTERVAL{24 * 60 * 60};
Mutex cs_unformable_log;
std::map<Consensus::LLMQType, int64_t> unformable_log_time GUARDED_BY(cs_unformable_log);

bool ShouldLogUnformableQuorum(Consensus::LLMQType llmqType)
{
    const int64_t now{GetTime()};
    LOCK(cs_unformable_log);
    if (auto& last = unformable_log_time[llmqType]; now - last >= UNFORMABLE_QUORUM_LOG_INTERVAL) {
        last = now;
        return true;
    }
    return false;
}
} // anonymous namespace

bool ActiveDKGSessionHandler::InitNewQuorum(gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex)
{
    if (!DeploymentDIP0003Enforced(pQuorumBaseBlockIndex->nHeight, Params().GetConsensus())) {
        return false;
    }

    // Quarantine gate (WS-HEAL v2 5.2.3): decline DKG participation outright
    // (belt-and-braces under the ActiveContext::UpdatedBlockTip gate) so a
    // round already in flight aborts instead of contributing.
    if (recovery::IsQuarantined()) {
        return false;
    }

    curSession = std::make_unique<ActiveDKGSession>(m_bls_worker, m_dmnman, m_dkgdbgman, m_qdkgsman, m_mn_metaman,
                                                    m_qsnapman, m_mn_activeman, m_chainman, m_sporkman,
                                                    pQuorumBaseBlockIndex, params);

    if (!curSession->Init(m_mn_activeman.GetProTxHash(), quorumIndex)) {
        // Dash ships LLMQ types (llmq_100_67, llmq_400_60, llmq_400_85, the rotating llmq_60_75)
        // that can never gather minSize members on a network this small. Logging their failure at
        // the default level on every round buries real DKG problems, so demote the "too few
        // eligible masternodes" case and keep one summary line per type per day.
        const size_t member_count{curSession->members.size()};
        if (member_count < static_cast<size_t>(params.minSize)) {
            LogPrint(BCLog::LLMQ_DKG, /* Continued */
                     "ActiveDKGSessionHandler::%s -- height[%d] quorum initialization failed for %s qi[%d]: "
                     "%d eligible masternodes < minSize %d\n",
                     __func__, pQuorumBaseBlockIndex->nHeight, curSession->params.name, quorumIndex, member_count,
                     params.minSize);
            if (ShouldLogUnformableQuorum(params.type)) {
                LogPrintf("llmq type %s cannot form: %d eligible masternodes < minSize %d\n", params.name,
                          member_count, params.minSize);
            }
        } else {
            LogPrintf("ActiveDKGSessionHandler::%s -- height[%d] quorum initialization failed for %s qi[%d]\n",
                      __func__, pQuorumBaseBlockIndex->nHeight, curSession->params.name, quorumIndex);
        }
        return false;
    }

    LogPrintf("ActiveDKGSessionHandler::%s -- height[%d] quorum initialization OK for %s qi[%d]\n", __func__, pQuorumBaseBlockIndex->nHeight, curSession->params.name, quorumIndex);
    return true;
}

class AbortPhaseException : public std::exception {
};

void ActiveDKGSessionHandler::WaitForNextPhase(std::optional<QuorumPhase> curPhase, QuorumPhase nextPhase,
                                               const uint256& expectedQuorumHash, const WhileWaitFunc& shouldNotWait) const
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, curPhase.has_value() ? ToUnderlying(*curPhase) : -1, ToUnderlying(nextPhase));

    while (true) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [_phase, _quorumHash] = GetPhaseAndQuorumHash();
        if (!expectedQuorumHash.IsNull() && _quorumHash != expectedQuorumHash) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected expectedQuorumHash change\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        if (_phase == nextPhase) {
            break;
        }
        if (curPhase.has_value() && _phase != curPhase) {
            // Burst tolerance (v1.3.2): ActivateBestChain connects several blocks
            // in one batch and fires a single UpdatedBlockTip at the final tip, so
            // the phase scalar can jump forward PAST nextPhase (e.g. Contribute ->
            // Justify) without ever passing through the intermediate value. The
            // expectedQuorumHash guard above already proved this is the same round,
            // and QuorumPhase is monotonic 1..7, so a value greater than curPhase
            // is a forward jump, never corruption. Do NOT abort the whole 24-block
            // round: break and let the linear HandleDKGRound chain run each skipped
            // phase's mandatory start handler (Contribute/Complain/Justify/Commit)
            // in order -- every following WaitForNextPhase then returns immediately
            // because phase is already at/after its target. A member too far behind
            // to have verified contributions abstains cleanly at SendCommitment;
            // this only rescues members that can still legitimately participate.
            if (ToUnderlying(_phase) > ToUnderlying(*curPhase)) {
                LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - arriving late, phase jumped forward _phase=%d, curPhase=%d, nextPhase=%d, catching up\n", __func__, params.name, quorumIndex, ToUnderlying(_phase), ToUnderlying(*curPhase), ToUnderlying(nextPhase));
                break;
            }
            // Genuine backward move within the same quorumHash (intra-window reorg).
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due backward phase change, _phase=%d, curPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(_phase), ToUnderlying(*curPhase));
            throw AbortPhaseException();
        }
        if (!shouldNotWait()) {
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, curPhase.has_value() ? ToUnderlying(*curPhase) : -1, ToUnderlying(nextPhase));

    if (nextPhase == QuorumPhase::Initialized) {
        m_dkgdbgman.ResetLocalSessionStatus(params.type, quorumIndex);
    } else {
        m_dkgdbgman.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
            bool changed = status.phase != nextPhase;
            status.phase = nextPhase;
            return changed;
        });
    }
}

void ActiveDKGSessionHandler::WaitForNewQuorum(const uint256& oldQuorumHash) const
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d]- starting\n", __func__, params.name, quorumIndex);

    while (true) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [_, _quorumHash] = GetPhaseAndQuorumHash();
        if (_quorumHash != oldQuorumHash) {
            break;
        }
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done\n", __func__, params.name, quorumIndex);
}

// Sleep some time to not fully overload the whole network
void ActiveDKGSessionHandler::SleepBeforePhase(QuorumPhase curPhase, const uint256& expectedQuorumHash,
                                               double randomSleepFactor, const WhileWaitFunc& runWhileWaiting) const
{
    if (!curSession->AreWeMember()) {
        // Non-members do not participate and do not create any network load, no need to sleep.
        return;
    }

    if (Params().MineBlocksOnDemand()) {
        // On regtest, blocks can be mined on demand without any significant time passing between these.
        // We shouldn't wait before phases in this case.
        return;
    }

    // Two blocks can come very close to each other, this happens pretty regularly. We don't want to be
    // left behind and marked as a bad member. This means that we should not count the last block of the
    // phase as a safe one to keep sleeping, that's why we calculate the phase sleep time as a time of
    // the full phase minus one block here.
    double phaseSleepTime = (params.dkgPhaseBlocks - 1) * Params().GetConsensus().nPowTargetSpacing * 1000;
    // Expected phase sleep time per member
    double phaseSleepTimePerMember = phaseSleepTime / params.size;
    // Don't expect perfect block times and thus reduce the phase time to be on the secure side (caller chooses factor)
    double adjustedPhaseSleepTimePerMember = phaseSleepTimePerMember * randomSleepFactor;

    int64_t sleepTime = (int64_t)(adjustedPhaseSleepTimePerMember * curSession->GetMyMemberIndex().value_or(0));
    int64_t endTime = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()) + sleepTime;
    int heightTmp{currentHeight.load()};
    int heightStart{heightTmp};

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting sleep for %d ms, curPhase=%d\n", __func__, params.name, quorumIndex, sleepTime, ToUnderlying(curPhase));

    while (TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()) < endTime) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto cur_height = currentHeight.load();
        if (cur_height > heightTmp) {
            // New block(s) just came in
            int64_t expectedBlockTime = (cur_height - heightStart) * Params().GetConsensus().nPowTargetSpacing * 1000;
            if (expectedBlockTime > sleepTime) {
                // Blocks came faster than we expected, jump into the phase func asap
                break;
            }
            heightTmp = cur_height;
        }
        {
            auto [p, qh] = GetPhaseAndQuorumHash();
            if (qh != expectedQuorumHash) {
                // A genuinely new round/quorum started under us.
                LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected expectedQuorumHash change\n", __func__, params.name, quorumIndex);
                throw AbortPhaseException();
            }
            if (ToUnderlying(p) < ToUnderlying(curPhase)) {
                // Backward move within the same round (intra-window reorg).
                LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due backward phase change, phase=%d, curPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(p), ToUnderlying(curPhase));
                throw AbortPhaseException();
            }
            if (p != curPhase) {
                // Burst tolerance (v1.3.2): blocks connected in one batch pushed the
                // phase past curPhase while we were sleeping. It is too late to keep
                // staggering -- stop sleeping and run this phase's start handler now
                // (mirrors the forward-jump break in WaitForNextPhase). Non-consensus:
                // only affects local DKG message scheduling.
                break;
            }
        }
        if (!runWhileWaiting()) {
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(curPhase));
}

// Keep a minimum amount of wall clock between two consecutive phase start handlers of one
// round. A multi-algo block burst can connect a whole phase in a couple of seconds, and a
// start handler such as VerifyAndComplain judges every member on what has arrived so far:
// without a dwell, a member whose contribution is merely still in flight is marked bad and
// gets PoSe-punished for being slow rather than for misbehaving. This is why the dwell is
// applied even when the chain has already jumped several phases ahead - that is exactly the
// case it exists for. It is purely local scheduling: no change to block validity and nothing
// different on the wire.
// Reads "1.3.3" out of a "/Kerrigan:1.3.3(comment)/" user agent. Anything we cannot read is
// treated as "not dwell-aware", which is the safe direction: it only ever keeps the dwell off.
static bool ParseKerriganVersion(const std::string& sub_ver, int& major, int& minor, int& patch)
{
    static const std::string prefix{"/Kerrigan:"};
    const size_t start{sub_ver.find(prefix)};
    if (start == std::string::npos) {
        return false;
    }
    const size_t vstart{start + prefix.size()};
    const size_t end{sub_ver.find_first_of("(/", vstart)};
    if (end == std::string::npos || end <= vstart) {
        return false;
    }
    const std::vector<std::string> parts{SplitString(sub_ver.substr(vstart, end - vstart), '.')};
    if (parts.size() < 3) {
        return false;
    }
    return ParseInt32(parts[0], &major) && ParseInt32(parts[1], &minor) && ParseInt32(parts[2], &patch);
}

// Cumulative, process-wide record of which masternodes have been seen advertising a client
// that honours the dwell, keyed by proTxHash. It is deliberately never pruned within a
// process: the survey is taken right after a phase boundary, and a block burst crosses that
// boundary in milliseconds - long before MNAUTH has finished on every link - so a count that
// moved with connection timing would put different nodes on different sides of the threshold
// during the same round, which is the split the arming rule exists to prevent. Growing only,
// it is stable across a round and converges between nodes within hours under SPORK_21.
// Shared by every quorum type's handler because a masternode's client version is a property
// of the node, not of the quorum we happened to meet it in.
static Mutex cs_dwell_aware;
static Uint256HashMap<bool> g_dwell_aware GUARDED_BY(cs_dwell_aware);

// Decide once per round whether the dwell applies. A dwell that only some members honour is
// worse than no dwell at all: the dwellers accept a late contribution the others already
// judged bad, the two groups then mine different bitsets, and if neither reaches minSize the
// round produces no commitment and everybody who was dropped takes a PoSe penalty anyway. So
// in `auto` we arm only when the quorum is overwhelmingly on a client that dwells too, and we
// re-decide every round so the network converges on its own as operators upgrade.
// Fold whatever verified masternode peers we can see right now into the record. Called more
// than once per round: a member can be connected while the quorum is being brought up and
// gone again by the time the survey runs, and the whole point of the record is that such a
// member is still counted.
void ActiveDKGSessionHandler::RefreshDwellAwareness(const CConnman& connman) const
{
    // Every verified masternode peer is recorded, not just members of this quorum, because
    // the next round draws a different membership from the same network.
    connman.ForEachNode([&](const CNode* pnode) {
        const uint256 proTxHash{pnode->GetVerifiedProRegTxHash()};
        if (proTxHash.IsNull()) {
            return;
        }
        const std::string sub_ver{WITH_LOCK(pnode->m_subver_mutex, return pnode->cleanSubVer)};
        int major{0}, minor{0}, patch{0};
        const bool aware{ParseKerriganVersion(sub_ver, major, minor, patch) &&
                         (major > DKG_PHASE_DWELL_MIN_MAJOR ||
                          (major == DKG_PHASE_DWELL_MIN_MAJOR &&
                           (minor > DKG_PHASE_DWELL_MIN_MINOR ||
                            (minor == DKG_PHASE_DWELL_MIN_MINOR && patch >= DKG_PHASE_DWELL_MIN_PATCH))))};
        // A member that comes back on an older client flips its flag back, but a member we
        // have already met is never dropped from the record.
        LOCK(cs_dwell_aware);
        g_dwell_aware[proTxHash] = aware;
    });
}

bool ActiveDKGSessionHandler::ComputeDwellArmed(const CConnman& connman) const
{
    RefreshDwellAwareness(connman);

    // Count DISTINCT members of the CURRENT quorum. Counting connections instead would be
    // wrong twice over: an inbound and an outbound link to the same masternode are two CNodes
    // but one member, and a member free to churn connections could buy itself votes.
    int dwell_aware{0};
    {
        LOCK(cs_dwell_aware);
        for (const auto& m : curSession->members) {
            if (m->dmn->proTxHash == curSession->myProTxHash) {
                ++dwell_aware; // ourselves, by construction
                continue;
            }
            if (const auto it = g_dwell_aware.find(m->dmn->proTxHash);
                it != g_dwell_aware.end() && it->second) {
                ++dwell_aware;
            }
        }
    }

    // minSize + margin keeps us clear of the smallest bitset that can still win a round. On a
    // quorum too small for that (regtest) require every member instead.
    const int needed{std::min(params.minSize + DKG_PHASE_DWELL_ARM_MARGIN, params.size)};
    bool armed{false};
    switch (m_dkg_phase_dwell_mode) {
    case DkgPhaseDwellMode::Off:
        armed = false;
        break;
    case DkgPhaseDwellMode::On:
        armed = true;
        break;
    case DkgPhaseDwellMode::Auto:
        armed = dwell_aware >= needed;
        break;
    }
    // Logged in every mode, once per round: the survey is what an operator needs to see to
    // know why `auto` is or is not dwelling yet, and it is cheap next to a round of BLS.
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - dwell %s for this round, dwell-aware members=%d of %d, needed=%d\n", __func__, params.name, quorumIndex, armed ? "armed" : "NOT armed", dwell_aware, (int)curSession->members.size(), needed);
    return armed;
}

void ActiveDKGSessionHandler::WaitPhaseDwell(QuorumPhase curPhase, const uint256& expectedQuorumHash,
                                             const WhileWaitFunc& runWhileWaiting) const
{
    if (m_dkg_phase_dwell <= 0 || !m_dwell_armed.load() || !curSession->AreWeMember()) {
        // A non-member judges nobody, so it has nothing to wait for and should not hold back
        // its own (observer-only) progress.
        return;
    }
    const int64_t prev_start_ms{m_phase_start_ms.load()};
    if (prev_start_ms == 0) {
        // First start handler of this round (Contribute). Never delay it: nothing has been
        // judged yet and delaying it would only make our own messages later for everybody.
        return;
    }

    const int64_t spacing{Params().GetConsensus().nPowTargetSpacing};
    const int64_t dwell_s{std::min({int64_t{m_dkg_phase_dwell}, int64_t{MAX_DKG_PHASE_DWELL},
                                    int64_t{params.dkgPhaseBlocks} * spacing / 2})};
    if (dwell_s <= 0) {
        return;
    }

    // Budget the dwell against the mining window. Commit is the last start handler of the
    // round, so the dwells still ahead of us must all fit before the window opens - a
    // commitment that misses its window helps nobody, which is strictly worse than judging
    // a little early.
    const int remaining_dwells{ToUnderlying(QuorumPhase::Commit) - ToUnderlying(curPhase) + 1};
    if (remaining_dwells <= 0) {
        return;
    }
    const int64_t blocks_left{int64_t{curSession->BlockIndex()->nHeight} + params.dkgMiningWindowStart -
                              currentHeight.load()};
    const int64_t budget_ms{blocks_left > 0 ? blocks_left * spacing * 1000 : 0};
    const int64_t planned_ms{std::min<int64_t>(dwell_s * 1000 * remaining_dwells, budget_ms)};
    const int64_t this_dwell_ms{planned_ms / remaining_dwells};
    if (this_dwell_ms <= 0) {
        LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - dwell skipped, mining window is too close (blocks_left=%d)\n", __func__, params.name, quorumIndex, blocks_left);
        return;
    }
    const int64_t end_time{prev_start_ms + this_dwell_ms};

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - dwelling up to %d ms before the next start handler (curPhase=%d, remaining=%d)\n", __func__, params.name, quorumIndex, end_time - GetTime<std::chrono::milliseconds>().count(), ToUnderlying(curPhase), remaining_dwells);

    while (GetTime<std::chrono::milliseconds>().count() < end_time) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [p, qh] = GetPhaseAndQuorumHash();
        if (qh != expectedQuorumHash) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected expectedQuorumHash change\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        if (ToUnderlying(p) < ToUnderlying(curPhase)) {
            // Backward move within the same round (intra-window reorg), same rule as
            // SleepBeforePhase: the session we were about to act on no longer exists.
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due backward phase change, phase=%d, curPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(p), ToUnderlying(curPhase));
            throw AbortPhaseException();
        }
        // A phase that has run FORWARD must not abort the dwell, otherwise the burst we are
        // protecting against would cancel the protection.
        if (!runWhileWaiting()) {
            UninterruptibleSleep(std::chrono::milliseconds{100});
        }
    }

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - dwell done\n", __func__, params.name, quorumIndex);
}

void ActiveDKGSessionHandler::HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase,
                                          const uint256& expectedQuorumHash, double randomSleepFactor,
                                          const StartPhaseFunc& startPhaseFunc, const WhileWaitFunc& runWhileWaiting)
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(curPhase), ToUnderlying(nextPhase));

    SleepBeforePhase(curPhase, expectedQuorumHash, randomSleepFactor, runWhileWaiting);
    WaitPhaseDwell(curPhase, expectedQuorumHash, runWhileWaiting);
    // Stamp before running the handler, not after: the dwell measures the gap between the
    // START of two handlers, and a handler that itself takes seconds (VerifyAndComplain on a
    // 60-member quorum does real BLS work) would otherwise silently extend every later dwell.
    m_phase_start_ms = GetTime<std::chrono::milliseconds>().count();
    startPhaseFunc();
    WaitForNextPhase(curPhase, nextPhase, expectedQuorumHash, runWhileWaiting);

    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - done, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(curPhase), ToUnderlying(nextPhase));
}

// returns a set of NodeIds which sent invalid messages
template<typename Message>
std::set<NodeId> BatchVerifyMessageSigs(CDKGSession& session, const std::vector<std::pair<NodeId, std::shared_ptr<Message>>>& messages)
{
    if (messages.empty()) {
        return {};
    }

    std::set<NodeId> ret;
    bool revertToSingleVerification = false;

    CBLSSignature aggSig;
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<uint256> messageHashes;
    std::set<uint256> messageHashesSet;
    pubKeys.reserve(messages.size());
    messageHashes.reserve(messages.size());
    bool first = true;
    for (const auto& [nodeId, msg] : messages) {
        auto member = session.GetMember(msg->proTxHash);
        if (!member) {
            // should not happen as it was verified before
            ret.emplace(nodeId);
            continue;
        }

        if (first) {
            aggSig = msg->sig;
        } else {
            aggSig.AggregateInsecure(msg->sig);
        }
        first = false;

        auto msgHash = msg->GetSignHash();
        if (!messageHashesSet.emplace(msgHash).second) {
            // can only happen in 2 cases:
            // 1. Someone sent us the same message twice but with differing signature, meaning that at least one of them
            //    must be invalid. In this case, we'd have to revert to single message verification nevertheless
            // 2. Someone managed to find a way to create two different binary representations of a message that deserializes
            //    to the same object representation. This would be some form of malleability. However, this shouldn't be
            //    possible as only deterministic/unique BLS signatures and very simple data types are involved
            revertToSingleVerification = true;
            break;
        }

        pubKeys.emplace_back(member->dmn->pdmnState->pubKeyOperator.Get());
        messageHashes.emplace_back(msgHash);
    }
    if (!revertToSingleVerification) {
        if (aggSig.VerifyInsecureAggregated(pubKeys, messageHashes)) {
            // all good
            return ret;
        }

        // are all messages from the same node?
        bool nodeIdsAllSame = std::adjacent_find( messages.begin(), messages.end(), [](const auto& first, const auto& second){
            return first.first != second.first;
        }) == messages.end();

        // if yes, take a short path and return a set with only him
        if (nodeIdsAllSame) {
            ret.emplace(messages[0].first);
            return ret;
        }
        // different nodes, let's figure out who are the bad ones
    }

    for (const auto& [nodeId, msg] : messages) {
        if (ret.count(nodeId)) {
            continue;
        }

        auto member = session.GetMember(msg->proTxHash);
        bool valid = msg->sig.VerifyInsecure(member->dmn->pdmnState->pubKeyOperator.Get(), msg->GetSignHash());
        if (!valid) {
            ret.emplace(nodeId);
        }
    }
    return ret;
}

static void RelayInvToParticipants(const CDKGSession& session, const CConnman& connman,
                                   const CSporkManager& sporkman, PeerManager& peerman, const CInv& inv)
{
    CDKGLogger logger(session, __func__, __LINE__);
    std::stringstream ss;
    const auto& relayMembers = session.RelayMembers();
    for (const auto& r : relayMembers) {
        ss << r.ToString().substr(0, 4) << " | ";
    }
    logger.Batch("RelayInvToParticipants inv[%s] relayMembers[%d] GetNodeCount[%d] GetNetworkActive[%d] "
                 "HasMasternodeQuorumNodes[%d] for quorumHash[%s] forMember[%s] relayMembers[%s]",
                 inv.ToString(), relayMembers.size(), connman.GetNodeCount(ConnectionDirection::Both),
                 connman.GetNetworkActive(),
                 connman.HasMasternodeQuorumNodes(session.GetType(), session.BlockIndex()->GetBlockHash()),
                 session.BlockIndex()->GetBlockHash().ToString(), session.ProTx().ToString().substr(0, 4), ss.str());

    // With SPORK_21 (all members connected) on, every member of this quorum is already an
    // intra-quorum peer of ours, so there is no reason to make the inv walk the 5-in/5-out
    // relay ring hop by hop: hand it to every connected member directly. Under a block burst
    // those saved hops are the difference between a message that lands before the next phase
    // start handler judges its sender and one that does not. Message handling is unchanged,
    // so v1.3.1 and v1.3.2 peers simply learn about the message sooner.
    const bool all_members_connected{IsAllMembersConnectedEnabled(session.GetType(), sporkman)};

    std::stringstream ss2;
    connman.ForEachNode([&](const CNode* pnode) {
        const uint256 proTxHash{pnode->GetVerifiedProRegTxHash()};
        const bool is_relay_member{!proTxHash.IsNull() && relayMembers.count(proTxHash) != 0};
        const bool is_connected_member{all_members_connected && !proTxHash.IsNull() &&
                                       session.GetMember(proTxHash) != nullptr};
        if (pnode->qwatch || is_relay_member || is_connected_member) {
            peerman.PushInventory(pnode->GetId(), inv);
        }

        if (proTxHash.IsNull()) {
            logger.Batch("node[%d:%s] not mn", pnode->GetId(), pnode->m_addr_name);
        } else if (!is_relay_member && !is_connected_member) {
            // Only peers we did NOT push to belong in this list. Before the one-hop relay
            // existed every non-ring peer was skipped, so the two sets coincided.
            ss2 << proTxHash.ToString().substr(0, 4) << " | ";
        }
    });
    logger.Batch("forMember[%s] NOTrelayMembers[%s]", session.ProTx().ToString().substr(0, 4), ss2.str());
    logger.Flush();
}

template <typename Message>
bool ProcessPendingMessageBatch(const CConnman& connman, const CSporkManager& sporkman, CDKGSession& session,
                                CDKGPendingMessages& pendingMessages, PeerManager& peerman, size_t maxCount)
{
    auto msgs = pendingMessages.PopAndDeserializeMessages<Message>(maxCount);
    if (msgs.empty()) {
        return false;
    }

    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> preverifiedMessages;
    preverifiedMessages.reserve(msgs.size());

    for (const auto& p : msgs) {
        const NodeId &nodeId = p.first;
        if (!p.second) {
            LogPrint(BCLog::LLMQ_DKG, "%s -- failed to deserialize message, peer=%d\n", __func__, nodeId);
            {
                pendingMessages.Misbehaving(nodeId, 100, peerman);
            }
            continue;
        }
        bool ban = false;
        if (!session.PreVerifyMessage(*p.second, ban)) {
            if (ban) {
                LogPrint(BCLog::LLMQ_DKG, "%s -- banning node due to failed preverification, peer=%d\n", __func__, nodeId);
                {
                    pendingMessages.Misbehaving(nodeId, 100, peerman);
                }
            }
            LogPrint(BCLog::LLMQ_DKG, "%s -- skipping message due to failed preverification, peer=%d\n", __func__, nodeId);
            continue;
        }
        preverifiedMessages.emplace_back(p);
    }
    if (preverifiedMessages.empty()) {
        return true;
    }

    auto badNodes = BatchVerifyMessageSigs(session, preverifiedMessages);
    if (!badNodes.empty()) {
        for (auto nodeId : badNodes) {
            LogPrint(BCLog::LLMQ_DKG, "%s -- failed to verify signature, peer=%d\n", __func__, nodeId);
            pendingMessages.Misbehaving(nodeId, 100, peerman);
        }
    }

    for (const auto& p : preverifiedMessages) {
        const NodeId &nodeId = p.first;
        if (badNodes.count(nodeId)) {
            continue;
        }
        const std::optional<CInv> inv = session.ReceiveMessage(*p.second);
        if (inv) {
            RelayInvToParticipants(session, connman, sporkman, peerman, *inv);
        }
    }

    return true;
}

// Empty a pending queue before the start handler judges on what it holds. The loop is
// bounded: a well-behaved quorum can produce at most two messages per member (the second
// one being the duplicate that condemns its sender), so anything beyond that is either a
// flood or a bug, and either way we must not spin here while the round moves on. Every
// pass re-checks the same abort conditions the sleep loops use.
template <typename Message>
void ActiveDKGSessionHandler::DrainPendingMessages(const CConnman& connman, CDKGPendingMessages& pendingMessages,
                                                   PeerManager& peerman, const uint256& expectedQuorumHash)
{
    const int max_batches{params.size * 2 / 8 + 1};
    for (int i = 0; i < max_batches; ++i) {
        if (stopRequested) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due to stop/shutdown requested\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        auto [_, _quorumHash] = GetPhaseAndQuorumHash();
        if (_quorumHash != expectedQuorumHash) {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborting due unexpected expectedQuorumHash change\n", __func__, params.name, quorumIndex);
            throw AbortPhaseException();
        }
        if (!ProcessPendingMessageBatch<Message>(connman, m_sporkman, *curSession, pendingMessages, peerman, 8)) {
            return;
        }
    }
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - drain stopped at the %d batch cap, queue still has messages\n", __func__, params.name, quorumIndex, max_batches);
}

void ActiveDKGSessionHandler::HandleDKGRound(CConnman& connman, PeerManager& peerman)
{
    WaitForNextPhase(std::nullopt, QuorumPhase::Initialized);

    pendingContributions.Clear();
    pendingComplaints.Clear();
    pendingJustifications.Clear();
    pendingPrematureCommitments.Clear();
    // The dwell is per round: the first start handler of a new round must never be delayed,
    // and whether it applies at all is re-decided below once the quorum is connected.
    m_phase_start_ms = 0;
    m_dwell_armed = false;
    uint256 curQuorumHash = WITH_LOCK(cs_phase_qhash, return quorumHash);

    const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(::cs_main,
                                                         return m_chainman.m_blockman.LookupBlockIndex(curQuorumHash));

    if (!pQuorumBaseBlockIndex || !InitNewQuorum(pQuorumBaseBlockIndex)) {
        // should actually never happen
        WaitForNewQuorum(curQuorumHash);
        throw AbortPhaseException();
    }

    m_dkgdbgman.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
        bool changed = status.phase != QuorumPhase::Initialized;
        status.phase = QuorumPhase::Initialized;
        return changed;
    });

    if (params.is_single_member()) {
        auto finalCommitment = curSession->FinalizeSingleCommitment();
        if (!finalCommitment.IsNull()) { // it can be null only if we are not member
            if (auto inv_opt = m_qblockman.AddMineableCommitment(finalCommitment); inv_opt.has_value()) {
                peerman.RelayInv(inv_opt.value());
            }
        }
        WaitForNextPhase(QuorumPhase::Initialized, QuorumPhase::Contribute, curQuorumHash);
        return;
    }

    const auto tip_mn_list = m_dmnman.GetListAtChainTip();
    utils::EnsureQuorumConnections(params, connman, m_sporkman, {m_dmnman, m_qsnapman, m_chainman, pQuorumBaseBlockIndex},
                                   tip_mn_list, curSession->myProTxHash, /*is_masternode=*/true, m_quorums_watch);
    if (curSession->AreWeMember()) {
        utils::AddQuorumProbeConnections(params, connman, m_mn_metaman, m_sporkman,
                                         {m_dmnman, m_qsnapman, m_chainman, pQuorumBaseBlockIndex}, tip_mn_list,
                                         curSession->myProTxHash);
    }

    // First look, while the connections we just asked for are coming up. The arming decision
    // is still taken once, below, but a member seen only during this window is remembered.
    RefreshDwellAwareness(connman);

    // ... and keep looking for the rest of the Initialized phase. MNAUTH completes at its own
    // pace on each link, so a single snapshot systematically undercounts: whichever members
    // had not yet identified themselves at that instant are invisible, and on a network where
    // that instant differs per node the arming decision would differ per node too. Sampling
    // throughout costs one walk of the peer list per 100 ms of a phase that happens once an
    // hour. If a burst crosses the boundary immediately this simply does not run, which is
    // why the two explicit calls around it stay.
    WaitForNextPhase(QuorumPhase::Initialized, QuorumPhase::Contribute, curQuorumHash,
                     [this, &connman] {
                         RefreshDwellAwareness(connman);
                         return false; // keep the 100 ms cadence of the wait loop
                     });

    // Decided here, after the Initialized phase has had its blocks to bring the intra-quorum
    // connections up, so the version survey sees the real membership.
    // Non-members judge nobody, so there is nothing to arm and no reason to survey.
    m_dwell_armed = curSession->AreWeMember() && ComputeDwellArmed(connman);

    // Contribute
    auto fContributeStart = [this, &peerman]() { curSession->Contribute(pendingContributions, peerman); };
    auto fContributeWait = [this, &connman, &peerman] {
        return ProcessPendingMessageBatch<CDKGContribution>(connman, m_sporkman, *curSession, pendingContributions, peerman, 8);
    };
    HandlePhase(QuorumPhase::Contribute, QuorumPhase::Complain, curQuorumHash, 0.05, fContributeStart, fContributeWait);

    // Complain
    // Drain every contribution that is still queued BEFORE judging who failed to send one.
    // After a block burst the queue routinely holds contributions that arrived while we were
    // being pushed across the phase boundary; judging without draining marks their senders
    // bad for a message we are holding in our own hands.
    auto fComplainStart = [this, &connman, &peerman, curQuorumHash]() {
        DrainPendingMessages<CDKGContribution>(connman, pendingContributions, peerman, curQuorumHash);
        curSession->VerifyAndComplain(connman, pendingComplaints, peerman);
    };
    // The wait callback deliberately processes ONLY complaints. Once VerifyAndComplain has
    // run, a contribution that arrives can no longer rescue its sender - bad is sticky - and
    // the only path still open to it is the duplicate-contribution path, which condemns.
    // Accepting late contributions here would therefore let a third party forge an exclusion
    // we had already decided against, so this must stay exactly as v1.3.2.
    auto fComplainWait = [this, &connman, &peerman] {
        return ProcessPendingMessageBatch<CDKGComplaint>(connman, m_sporkman, *curSession, pendingComplaints, peerman, 8);
    };
    HandlePhase(QuorumPhase::Complain, QuorumPhase::Justify, curQuorumHash, 0.05, fComplainStart, fComplainWait);

    // Justify
    auto fJustifyStart = [this, &connman, &peerman, curQuorumHash]() {
        DrainPendingMessages<CDKGComplaint>(connman, pendingComplaints, peerman, curQuorumHash);
        curSession->VerifyAndJustify(pendingJustifications, peerman);
    };
    // Only justifications, for the same reason: VerifyAndJustify runs once, so a complaint
    // accepted after it can only ever add an entry that marks its target bad at commit time.
    auto fJustifyWait = [this, &connman, &peerman] {
        return ProcessPendingMessageBatch<CDKGJustification>(connman, m_sporkman, *curSession, pendingJustifications, peerman, 8);
    };
    HandlePhase(QuorumPhase::Justify, QuorumPhase::Commit, curQuorumHash, 0.05, fJustifyStart, fJustifyWait);

    // Commit
    auto fCommitStart = [this, &connman, &peerman, curQuorumHash]() {
        DrainPendingMessages<CDKGJustification>(connman, pendingJustifications, peerman, curQuorumHash);
        curSession->VerifyAndCommit(pendingPrematureCommitments, peerman);
    };
    auto fCommitWait = [this, &connman, &peerman] {
        return ProcessPendingMessageBatch<CDKGPrematureCommitment>(connman, m_sporkman, *curSession,
                                                                   pendingPrematureCommitments, peerman, 8);
    };
    HandlePhase(QuorumPhase::Commit, QuorumPhase::Finalize, curQuorumHash, 0.1, fCommitStart, fCommitWait);

    auto finalCommitments = curSession->FinalizeCommitments();
    for (const auto& fqc : finalCommitments) {
        if (auto inv_opt = m_qblockman.AddMineableCommitment(fqc); inv_opt.has_value()) {
            peerman.RelayInv(inv_opt.value());
        }
    }
}

void ActiveDKGSessionHandler::PhaseHandlerThread(CConnman& connman, PeerManager& peerman)
{
    while (!stopRequested) {
        try {
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting HandleDKGRound\n", __func__, params.name, quorumIndex);
            HandleDKGRound(connman, peerman);
        } catch (AbortPhaseException& e) {
            m_dkgdbgman.UpdateLocalSessionStatus(params.type, quorumIndex, [&](CDKGDebugSessionStatus& status) {
                status.statusBits.aborted = true;
                return true;
            });
            LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - aborted current DKG session\n", __func__, params.name, quorumIndex);
        }
    }
}

bool ActiveDKGSessionHandler::GetContribution(const uint256& hash, CDKGContribution& ret) const
{
    if (!curSession) {
        return false;
    }
    LOCK(curSession->invCs);
    auto it = curSession->contributions.find(hash);
    if (it != curSession->contributions.end()) {
        ret = it->second;
        return true;
    }
    return false;
}

bool ActiveDKGSessionHandler::GetComplaint(const uint256& hash, CDKGComplaint& ret) const
{
    if (!curSession) {
        return false;
    }
    LOCK(curSession->invCs);
    auto it = curSession->complaints.find(hash);
    if (it != curSession->complaints.end()) {
        ret = it->second;
        return true;
    }
    return false;
}

bool ActiveDKGSessionHandler::GetJustification(const uint256& hash, CDKGJustification& ret) const
{
    if (!curSession) {
        return false;
    }
    LOCK(curSession->invCs);
    auto it = curSession->justifications.find(hash);
    if (it != curSession->justifications.end()) {
        ret = it->second;
        return true;
    }
    return false;
}

bool ActiveDKGSessionHandler::GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const
{
    if (!curSession) {
        return false;
    }
    LOCK(curSession->invCs);
    auto it = curSession->prematureCommitments.find(hash);
    if (it != curSession->prematureCommitments.end() && curSession->validCommitments.count(hash)) {
        ret = it->second;
        return true;
    }
    return false;
}

QuorumPhase ActiveDKGSessionHandler::GetPhase() const
{
    return WITH_LOCK(cs_phase_qhash, return phase);
}
} // namespace llmq
