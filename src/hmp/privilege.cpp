// Copyright (c) 2026 Kerrigan Network
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hmp/privilege.h>

#include <consensus/params.h>
#include <hmp/identity.h>
#include <logging.h>
#include <primitives/block.h>

#include <algorithm>
#include <set>

namespace {
/** Map an HMPPrivilegeTier to a stable lowercase string for log lines.
 *  Kept TU-local so it does not collide with the static helper of the same
 *  name in src/rpc/hmp.cpp; both intentionally produce the identical mapping. */
const char* TierName(HMPPrivilegeTier tier)
{
    switch (tier) {
    case HMPPrivilegeTier::BROOD:   return "BROOD";
    case HMPPrivilegeTier::ELDER:   return "ELDER";
    case HMPPrivilegeTier::NEW:     return "NEW";
    case HMPPrivilegeTier::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}
} // namespace

CHMPPrivilegeTracker::CHMPPrivilegeTracker(const Consensus::Params& params)
    : m_windowSize(params.nHMPPrivilegeWindow),
      m_warmupBlocks(params.nHMPWarmupBlocks),
      m_minBlocksSolved(params.nHMPMinBlocksSolved),
      m_dominanceCatchFloor(params.nHMPDominanceCatchFloor),
      m_extendedWindowSize(params.nHMPDominanceCatchMaxLookback),
      m_consensus(&params)
{
}

void CHMPPrivilegeTracker::RebuildFromWindow()
{
    AssertLockHeld(cs);

    for (int a = 0; a < NUM_ALGOS; a++) {
        m_records[a].clear();
    }

    for (const auto& entry : m_window) {
        if (entry.algo >= 0 && entry.algo < NUM_ALGOS && entry.minerPubKey.IsValid()) {
            auto& rec = m_records[entry.algo][entry.minerPubKey];
            rec.blocks_solved++;
            rec.last_active_height = entry.height;
            if (rec.first_seen_height < 0 || entry.height < rec.first_seen_height) {
                rec.first_seen_height = entry.height;
            }
        }

        for (size_t si = 0; si < entry.sealSigners.size(); si++) {
            const auto& signer = entry.sealSigners[si];
            if (!signer.IsValid()) continue;
            // Seal participation: record under the signer's declared algo
            int signerAlgo = (si < entry.signerAlgos.size()) ? static_cast<int>(entry.signerAlgos[si]) : entry.algo;
            if (signerAlgo >= 0 && signerAlgo < NUM_ALGOS) {
                auto& rec = m_records[signerAlgo][signer];
                rec.seal_participations++;
                rec.last_active_height = std::max(rec.last_active_height, entry.height);
                if (rec.first_seen_height < 0) {
                    rec.first_seen_height = entry.height;
                }
            }
        }
    }
}

void CHMPPrivilegeTracker::BlockConnected(int height, int algo, const CBLSPublicKey& minerPubKey,
                                          const std::vector<CBLSPublicKey>& sealSigners,
                                          const std::vector<uint8_t>& signerAlgos,
                                          int sealedAlgo)
{
    LOCK(cs);

    CHMPWindowEntry entry;
    entry.height = height;
    entry.algo = algo;
    entry.minerPubKey = minerPubKey;
    entry.sealSigners = sealSigners;
    entry.signerAlgos = signerAlgos;
    entry.sealedAlgo = sealedAlgo;
    m_window.push_back(std::move(entry));

    // v1.3.2 §1: 400-block seal-presence lookback (arming HOLD condition).
    m_sealLong.push_back(CHMPSealLongEntry{height, algo, sealedAlgo});
    while (static_cast<int>(m_sealLong.size()) > SEAL_LONG_WINDOW) {
        m_sealLong.pop_front();
    }

    // Add to extended window (dominance catch, lightweight, solver identity only)
    if (minerPubKey.IsValid()) {
        CHMPExtendedEntry ext;
        ext.height = height;
        ext.algo = algo;
        ext.minerPubKey = minerPubKey;
        m_extendedWindow.push_back(std::move(ext));
        while (static_cast<int>(m_extendedWindow.size()) > m_extendedWindowSize) {
            m_extendedWindow.pop_front();
        }
    }

    bool evicted = false;
    while (static_cast<int>(m_window.size()) > m_windowSize) {
        m_window.pop_front();
        evicted = true;
    }

    if (evicted) {
        // Records may contain stale entries from evicted window slots, rebuild
        RebuildFromWindow();
    } else {
        // Safe to do incremental update (no entries were evicted)
        if (algo >= 0 && algo < NUM_ALGOS && minerPubKey.IsValid()) {
            auto& rec = m_records[algo][minerPubKey];
            rec.blocks_solved++;
            rec.last_active_height = height;
            if (rec.first_seen_height < 0) {
                rec.first_seen_height = height;
            }
        }

        for (size_t si = 0; si < sealSigners.size(); si++) {
            const auto& signer = sealSigners[si];
            if (!signer.IsValid()) continue;
            // Seal participation: record under the signer's declared algo
            int signerAlgo = (si < signerAlgos.size()) ? static_cast<int>(signerAlgos[si]) : algo;
            if (signerAlgo >= 0 && signerAlgo < NUM_ALGOS) {
                auto& rec = m_records[signerAlgo][signer];
                rec.seal_participations++;
                rec.last_active_height = std::max(rec.last_active_height, height);
                if (rec.first_seen_height < 0) {
                    rec.first_seen_height = height;
                }
            }
        }
    }
}

void CHMPPrivilegeTracker::BlockDisconnected(int height)
{
    LOCK(cs);

    while (!m_window.empty() && m_window.back().height >= height) {
        m_window.pop_back();
    }

    while (!m_extendedWindow.empty() && m_extendedWindow.back().height >= height) {
        m_extendedWindow.pop_back();
    }

    while (!m_sealLong.empty() && m_sealLong.back().height >= height) {
        m_sealLong.pop_back();
    }

    RebuildFromWindow();
}

HMPPrivilegeTier CHMPPrivilegeTracker::GetTier(const CBLSPublicKey& pubkey, int algo,
                                                const CBlockIndex* pindex) const
{
    LOCK(cs);

    if (algo < 0 || algo >= NUM_ALGOS) return HMPPrivilegeTier::UNKNOWN;

    auto it = m_records[algo].find(pubkey);
    if (it == m_records[algo].end()) return HMPPrivilegeTier::UNKNOWN;

    const auto& rec = it->second;
    int currentHeight = GetTipHeight();

    // Check warmup: must have been seen for at least m_warmupBlocks
    if (rec.first_seen_height < 0 || (currentHeight - rec.first_seen_height) < m_warmupBlocks) {
        return HMPPrivilegeTier::UNKNOWN;
    }

    // Compute the tier for this (pubkey, algo) at the current tip.
    HMPPrivilegeTier tier = HMPPrivilegeTier::NEW;

    // Elder: solved >= effective threshold AND has seal participation.
    // The effective threshold is gated by consensus.nHMPSealAlgoFixHeight (v1.2.0):
    // pre-fork it returns the legacy nHMPMinBlocksSolved verbatim (preserving
    // existing chain semantics); post-fork it returns the recalibrated value.
    // When m_consensus is null (default-constructed tracker, test-only paths)
    // we fall back to the captured m_minBlocksSolved.
    const int effMinBlocks = (m_consensus != nullptr)
        ? m_consensus->GetEffectiveMinBlocksSolved(currentHeight)
        : m_minBlocksSolved;
    if (rec.blocks_solved >= effMinBlocks && rec.seal_participations > 0) {
        // (A-01: the equivocation-demotion branch that used to live here was dead
        // code -- nothing ever populated the demotion map -- and was removed in
        // v1.3.2. Consensus-inert: the branch could never fire.)
        if (m_mnLookup && m_mnLookup->IsBroodNodeOperator(pubkey, pindex)) {
            // BROOD: ELDER + active BroodNode operator
            // Pass pindex so the lookup uses the deterministic MN list at this
            // block rather than the (non-deterministic) chain tip.
            tier = HMPPrivilegeTier::BROOD;
        } else {
            tier = HMPPrivilegeTier::ELDER;
        }
    }
    // else: NEW (past warmup period, but below Elder threshold)

    // Operator-visibility: log NEW -> ELDER -> BROOD transitions on the canonical
    // tip-view path (pindex == nullptr). Consensus paths pass a historical pindex
    // and may legitimately disagree with the tip view (different MN list); those
    // observations would generate spurious transitions, so we only cache and log
    // when pindex is null. Local-node tier changes get a more prominent prefix
    // so operators spot their own node's promotion in the log stream.
    if (pindex == nullptr) {
        auto cacheIt = m_lastTier[algo].find(pubkey);
        if (cacheIt == m_lastTier[algo].end()) {
            // First observation -- seed the cache without logging.
            m_lastTier[algo][pubkey] = tier;
        } else if (cacheIt->second != tier) {
            const HMPPrivilegeTier prevTier = cacheIt->second;
            const bool isLocalNode = g_hmp_identity && g_hmp_identity->IsValid() &&
                                     pubkey == g_hmp_identity->GetPublicKey();
            if (isLocalNode) {
                LogPrintf("HMP: *** YOUR NODE *** tier %s -> %s on algo %s (height %d)\n",
                          TierName(prevTier),
                          TierName(tier),
                          GetAlgoName(algo),
                          currentHeight);
            } else {
                LogPrintf("HMP: signer %s... tier %s -> %s on algo %s (height %d)\n",
                          pubkey.ToString().substr(0, 16),
                          TierName(prevTier),
                          TierName(tier),
                          GetAlgoName(algo),
                          currentHeight);
            }
            cacheIt->second = tier;
        }
    }

    return tier;
}

uint32_t CHMPPrivilegeTracker::GetEffectiveWeight(const CBLSPublicKey& pubkey, int algo) const
{
    LOCK(cs);

    if (algo < 0 || algo >= NUM_ALGOS) return 0;

    auto it = m_records[algo].find(pubkey);
    if (it == m_records[algo].end()) return 0;

    // Total weight is sum of all blocks solved in this algo
    uint32_t totalWeight = 0;
    for (const auto& [pk, rec] : m_records[algo]) {
        totalWeight += rec.blocks_solved;
    }

    if (totalWeight == 0) return 0;

    uint32_t myWeight = it->second.blocks_solved;

    // Express as basis points (0-10000)
    uint32_t bps = (myWeight * 10000) / totalWeight;

    return std::min(bps, uint32_t{2500});
}

std::vector<CBLSPublicKey> CHMPPrivilegeTracker::GetExtendedSolvers(int algo, int count) const
{
    AssertLockHeld(cs);

    // Walk the extended window backwards to find the last N unique solvers for this algo
    std::vector<CBLSPublicKey> solvers;
    std::set<CBLSPublicKey> seen;

    for (auto it = m_extendedWindow.rbegin(); it != m_extendedWindow.rend(); ++it) {
        if (it->algo != algo) continue;
        if (!it->minerPubKey.IsValid()) continue;
        if (seen.count(it->minerPubKey)) continue;

        seen.insert(it->minerPubKey);
        solvers.push_back(it->minerPubKey);
        if (static_cast<int>(solvers.size()) >= count) break;
    }
    return solvers;
}

std::vector<CBLSPublicKey> CHMPPrivilegeTracker::GetElderSet(int algo, bool includeBackfill) const
{
    LOCK(cs);

    std::vector<CBLSPublicKey> result;
    if (algo < 0 || algo >= NUM_ALGOS) return result;

    int currentHeight = m_window.empty() ? -1 : m_window.back().height;

    // Normal Elders: solved + sealed within the standard window.
    // Threshold gated by consensus.nHMPSealAlgoFixHeight via GetEffectiveMinBlocksSolved
    // so this query stays in lockstep with GetTier() across the v1.2.0 activation.
    const int effMinBlocks = (m_consensus != nullptr)
        ? m_consensus->GetEffectiveMinBlocksSolved(currentHeight)
        : m_minBlocksSolved;
    std::set<CBLSPublicKey> elderSet;
    for (const auto& [pubkey, rec] : m_records[algo]) {
        if (rec.first_seen_height >= 0 &&
            (currentHeight - rec.first_seen_height) >= m_warmupBlocks &&
            rec.blocks_solved >= effMinBlocks &&
            rec.seal_participations > 0) {
            result.push_back(pubkey);
            elderSet.insert(pubkey);
        }
    }

    // Dominance catch (PRD 4.6.3): if fewer than floor, extend by including
    // recent unique solvers from the extended window who also have seal
    // participation in the standard window. Search broadly to find enough
    // qualified candidates since some may lack current seal participation.
    if (includeBackfill && static_cast<int>(result.size()) < m_dominanceCatchFloor && m_dominanceCatchFloor > 0) {
        // B-01 (v1.3.2, gated at nHMPRearmHeight): the backfill used to admit any
        // identity with blocks_solved >= 1, undercutting the recalibrated Elder
        // threshold and letting 1-block Sybils into the Elder set. Post-rearm the
        // backfill applies the same effective threshold as the organic path.
        const int backfillMinBlocks =
            (m_consensus != nullptr && m_consensus->IsHMPRearmActive(currentHeight)) ? effMinBlocks : 1;
        auto extSolvers = GetExtendedSolvers(algo, m_extendedWindowSize);
        for (const auto& pk : extSolvers) {
            if (elderSet.count(pk)) continue; // already an Elder

            // Check if this pool has both seal participation AND minimum blocks solved
            // in the standard window. seal_participations alone is insufficient:
            // a signing-only Sybil that mined one block in the extended window can
            // accumulate seal_participations without recent mining activity.
            auto it = m_records[algo].find(pk);
            if (it != m_records[algo].end() &&
                it->second.first_seen_height >= 0 &&
                (currentHeight - it->second.first_seen_height) >= m_warmupBlocks &&
                it->second.seal_participations > 0 &&
                it->second.blocks_solved >= backfillMinBlocks) {
                result.push_back(pk);
                elderSet.insert(pk);
            }

            if (static_cast<int>(result.size()) >= m_dominanceCatchFloor) break;
        }
    }

    return result;
}

std::vector<CBLSPublicKey> CHMPPrivilegeTracker::GetPrivilegedSet(int algo) const
{
    LOCK(cs);

    std::vector<CBLSPublicKey> result;
    if (algo < 0 || algo >= NUM_ALGOS) return result;

    int currentHeight = m_window.empty() ? -1 : m_window.back().height;

    for (const auto& [pubkey, rec] : m_records[algo]) {
        if (rec.first_seen_height >= 0 &&
            (currentHeight - rec.first_seen_height) >= m_warmupBlocks) {
            result.push_back(pubkey);
        }
    }
    return result;
}

size_t CHMPPrivilegeTracker::GetTotalPrivilegedCount() const
{
    LOCK(cs);

    std::set<CBLSPublicKey> unique;
    int currentHeight = m_window.empty() ? -1 : m_window.back().height;

    for (int a = 0; a < NUM_ALGOS; a++) {
        for (const auto& [pubkey, rec] : m_records[a]) {
            if (rec.first_seen_height >= 0 &&
                (currentHeight - rec.first_seen_height) >= m_warmupBlocks) {
                unique.insert(pubkey);
            }
        }
    }
    return unique.size();
}

CPrivilegeRecord CHMPPrivilegeTracker::GetRecord(const CBLSPublicKey& pubkey, int algo) const
{
    LOCK(cs);

    if (algo >= 0 && algo < NUM_ALGOS) {
        auto it = m_records[algo].find(pubkey);
        if (it != m_records[algo].end()) {
            return it->second;
        }
    }
    return CPrivilegeRecord{};
}

int CHMPPrivilegeTracker::GetTipHeight() const
{
    LOCK(cs);
    if (m_window.empty()) return -1;
    return m_window.back().height;
}

// v1.3.2 §1 arming constants (locked by the G1 simulator, see
// v132-locked-constants.md). Integer math only -- these are consensus.
namespace {
constexpr int ARM_FRAC_PCT      = 60; // sealed100 * 100 >= algoBlocks100 * 60
constexpr int ARM_MIN_SEALED    = 8;
constexpr int ARM_MIN_ELDERS    = 4;  // organic
constexpr int ARM_MIN_DIVERSITY = 4;  // distinct seal-signer identities in window
constexpr int HOLD_FRAC_PCT     = 45; // sealed400 * 100 >= algoBlocks400 * 45
constexpr int HOLD_MIN_ELDERS   = 3;  // organic
} // namespace

bool CHMPPrivilegeTracker::IsPenaltyArmed(int algo) const
{
    LOCK(cs);
    if (algo < 0 || algo >= NUM_ALGOS) return false;

    // Organic Elders only (no dominance-catch backfill) -- review F10.
    const int organicElders = static_cast<int>(GetElderSet(algo, /*includeBackfill=*/false).size());

    // 100-block window: this algo's blocks, how many carried a verified seal
    // covering an ancestor of this algo, and how many distinct identities signed.
    int algoBlocks100 = 0, sealed100 = 0;
    std::set<CBLSPublicKey> signers100;
    for (const auto& e : m_window) {
        if (e.algo == algo) algoBlocks100++;
        if (e.sealedAlgo == algo) {
            sealed100++;
            for (const auto& pk : e.sealSigners) {
                if (pk.IsValid()) signers100.insert(pk);
            }
        }
    }
    const bool arm = organicElders >= ARM_MIN_ELDERS &&
                     sealed100 >= ARM_MIN_SEALED &&
                     algoBlocks100 > 0 &&
                     sealed100 * 100 >= algoBlocks100 * ARM_FRAC_PCT &&
                     static_cast<int>(signers100.size()) >= ARM_MIN_DIVERSITY;
    if (arm) return true;

    // 400-block HOLD: keeps a healthy algo armed through short dips without any
    // path-dependent state (RebuildHMPState reconstructs both windows verbatim).
    int algoBlocks400 = 0, sealed400 = 0;
    for (const auto& e : m_sealLong) {
        if (e.algo == algo) algoBlocks400++;
        if (e.sealedAlgo == algo) sealed400++;
    }
    return organicElders >= HOLD_MIN_ELDERS &&
           algoBlocks400 > 0 &&
           sealed400 * 100 >= algoBlocks400 * HOLD_FRAC_PCT;
}

void CHMPPrivilegeTracker::Clear()
{
    LOCK(cs);
    m_window.clear();
    m_extendedWindow.clear();
    m_sealLong.clear();
    for (int a = 0; a < NUM_ALGOS; a++) {
        m_records[a].clear();
        m_lastTier[a].clear();
    }
    LogPrintf("HMP: privilege tracker cleared (activation boundary / cold rebuild)\n");
}
