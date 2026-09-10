// Copyright (c) 2026 The Kerrigan developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KERRIGAN_HMP_NEGATIVE_PROOF_H
#define KERRIGAN_HMP_NEGATIVE_PROOF_H

#include <cstdint>
#include <vector>

class CBLSPublicKey;
class CHMPPrivilegeTracker;

/**
 * Evaluate Elder absence and mannequin presence in a block's seal.
 * Detects attacker chains missing known Elders or filled with unknown signers (PRD SS4.7.4).
 * @return multiplier in basis points (10000 = neutral, >10000 = bonus, <10000 = penalty)
 */
uint64_t EvaluateNegativeProof(const std::vector<CBLSPublicKey>& signerPubKeys,
                                const std::vector<uint8_t>& signerAlgos,
                                int blockAlgo,
                                const CHMPPrivilegeTracker* privilege);

/**
 * v1.3.2 (>= nHMPRearmHeight): the ONLY sub-floor penalty left in the ladder.
 * Elder-ABSENCE penalties are gone post-rearm (the on-chain-verifiable committee
 * is exactly the set of signers with selected VRF proofs, so absence is not
 * observable; the unsealed-block floor carries the seal incentive instead).
 * What remains is the provable-fraud case: a seal whose signers are mostly
 * strangers to every privilege set ("mannequins", PRD SS4.7.4).
 * @return 1000 (0.1x) if > 50% of signers are unknown, else 10000 (neutral).
 */
uint64_t EvaluateMannequinPenalty(const std::vector<CBLSPublicKey>& signerPubKeys,
                                  const CHMPPrivilegeTracker* privilege);

#endif // KERRIGAN_HMP_NEGATIVE_PROOF_H
