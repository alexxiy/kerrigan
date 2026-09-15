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
    const CSporkManager& sporkman, const Consensus::LLMQParams& llmq_params, bool quorums_watch, int quorums_idx) :
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

void ActiveDKGSessionHandler::HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase,
                                          const uint256& expectedQuorumHash, double randomSleepFactor,
                                          const StartPhaseFunc& startPhaseFunc, const WhileWaitFunc& runWhileWaiting)
{
    LogPrint(BCLog::LLMQ_DKG, "ActiveDKGSessionHandler::%s -- %s qi[%d] - starting, curPhase=%d, nextPhase=%d\n", __func__, params.name, quorumIndex, ToUnderlying(curPhase), ToUnderlying(nextPhase));

    SleepBeforePhase(curPhase, expectedQuorumHash, randomSleepFactor, runWhileWaiting);
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

    WaitForNextPhase(QuorumPhase::Initialized, QuorumPhase::Contribute, curQuorumHash);

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
