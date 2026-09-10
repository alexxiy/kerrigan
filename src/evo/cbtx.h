// Copyright (c) 2017-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_CBTX_H
#define BITCOIN_EVO_CBTX_H

#include <bls/bls.h>
#include <hmp/seal.h>

#include <primitives/transaction.h>

#include <univalue.h>

#include <optional>
#include <string>

class BlockValidationState;
class CBlock;
class CBlockIndex;
class CDeterministicMNList;
class TxValidationState;
struct RPCResult;

namespace llmq {
class CQuorumBlockProcessor;
}// namespace llmq

// coinbase transaction
class CCbTx
{
public:
    enum class Version : uint16_t {
        INVALID = 0,
        MERKLE_ROOT_MNLIST = 1,
        MERKLE_ROOT_QUORUMS = 2,
        CLSIG_AND_BALANCE = 3,
        HMP_SEAL = 4,
        HMP_COMMITMENT = 5,
        HMP_REARM = 6,
        UNKNOWN,
    };

    static constexpr auto SPECIALTX_TYPE = TRANSACTION_COINBASE;
    Version nVersion{Version::MERKLE_ROOT_QUORUMS};
    int32_t nHeight{0};
    uint256 merkleRootMNList;
    uint256 merkleRootQuorums;
    uint32_t bestCLHeightDiff{0};
    CBLSSignature bestCLSignature;
    CAmount creditPoolBalance{0};

    // HMP_SEAL (v4) fields: Hivemind Protocol
    CBLSPublicKey minerIdentity;      // 48 bytes, this block's miner HMP pubkey
    CBLSSignature sealForAncestor;    // 96 bytes, assembled seal for block N-2
    uint256 sealBlockHash;            // 32 bytes, which block the seal covers
    // Signer list stored on-chain so nSealWeight is reproducible without in-memory seal cache
    std::vector<CBLSPublicKey> vSealSigners;    // signers of sealForAncestor
    std::vector<uint8_t> vSealSignerAlgos;      // per-signer algo domain (parallel to vSealSigners)

    // HMP_COMMITMENT (v5) fields: Phase 1 pubkey commitments
    std::vector<CBLSPublicKey> vCommitments; // max 16 per block = 768 bytes

    // HMP_REARM (v6) fields: on-chain proofs that make the seal-weight economics
    // verifiable and unforgeable (see the v1.3.2 design).
    //   B-02: per-seal-signer VRF proofs, index-parallel to vSealSigners. Lets
    //         every validator recompute the VRF committee for the block's seal and
    //         count only genuinely-selected signers toward weight/presence.
    //   B-03: per-commitment self-signatures, index-parallel to vCommitments.
    //         Proves the committer controls the pubkey it registers.
    //   Signed miner identity: signature by minerIdentity over block-binding data,
    //         so a miner cannot inflate another key's blocks_solved (Q3).
    std::vector<CBLSSignature> vSealSignerVRFProofs; // parallel to vSealSigners, <= MAX_SEAL_SIGNERS
    std::vector<CBLSSignature> vCommitmentSigs;      // parallel to vCommitments, <= 16
    CBLSSignature minerIdentitySig;                  // 96 bytes, sig by minerIdentity

    SERIALIZE_METHODS(CCbTx, obj)
    {
        READWRITE(obj.nVersion, obj.nHeight, obj.merkleRootMNList);

        if (obj.nVersion >= Version::MERKLE_ROOT_QUORUMS) {
            READWRITE(obj.merkleRootQuorums);
            if (obj.nVersion >= Version::CLSIG_AND_BALANCE) {
                READWRITE(COMPACTSIZE(obj.bestCLHeightDiff));
                READWRITE(obj.bestCLSignature);
                READWRITE(obj.creditPoolBalance);
                if (obj.nVersion >= Version::HMP_SEAL) {
                    READWRITE(obj.minerIdentity);
                    READWRITE(obj.sealForAncestor);
                    READWRITE(obj.sealBlockHash);
                    READWRITE(LIMITED_VECTOR(obj.vSealSigners, MAX_SEAL_SIGNERS));
                    READWRITE(LIMITED_VECTOR(obj.vSealSignerAlgos, MAX_SEAL_SIGNERS));
                    if (obj.nVersion >= Version::HMP_COMMITMENT) {
                        READWRITE(LIMITED_VECTOR(obj.vCommitments, 16));
                        if (obj.nVersion >= Version::HMP_REARM) {
                            READWRITE(LIMITED_VECTOR(obj.vSealSignerVRFProofs, MAX_SEAL_SIGNERS));
                            READWRITE(LIMITED_VECTOR(obj.vCommitmentSigs, 16));
                            READWRITE(obj.minerIdentitySig);
                        }
                    }
                }
            }
        }

    }

    /** v6 (HMP_REARM): message the miner signs with minerIdentity to prove it
     *  controls that key. Binds height + parent + the coinbase payout script: a
     *  copied (identity, sig) pair only validates in a block that pays the honest
     *  miner, so nobody can credit blocks_solved to a key they don't control (Q3).
     *  Shared by the miner (node/miner.cpp) and ConnectBlock; never diverge them. */
    static uint256 MinerIdentitySignatureHash(int32_t nHeight, const uint256& hashPrevBlock,
                                              const CScript& payoutScript);

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    std::string ToString() const;

    [[nodiscard]] UniValue ToJson() const;
};
template<> struct is_serializable_enum<CCbTx::Version> : std::true_type {};

bool CheckCbTx(const CCbTx& cbTx, const CBlockIndex* pindexPrev, TxValidationState& state);

bool CalcCbTxMerkleRootQuorums(const CBlock& block, const CBlockIndex* pindexPrev,
                               const llmq::CQuorumBlockProcessor& quorum_block_processor, uint256& merkleRootRet,
                               BlockValidationState& state);

std::optional<std::pair<CBLSSignature, uint32_t>> GetNonNullCoinbaseChainlock(const CBlockIndex* pindex);

#endif // BITCOIN_EVO_CBTX_H
