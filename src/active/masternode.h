// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ACTIVE_MASTERNODE_H
#define BITCOIN_ACTIVE_MASTERNODE_H

#include <bls/bls.h>

#include <netaddress.h>
#include <primitives/transaction.h>
#include <threadsafety.h>
#include <validationinterface.h>

#include <string>

class CConnman;
class CDeterministicMNManager;

/**
 * Default for -masternodeselfcheck. A failed connection to our own external address usually means
 * the local router does not support NAT hairpinning, not that the port is closed to the internet.
 * Warn by default (0) instead of refusing to run; 1 restores the pre-v1.3.3 fatal behaviour.
 */
static constexpr bool DEFAULT_MASTERNODE_SELF_CHECK{false};

/** Re-run the self-connect probe roughly once per hour of chain time. */
static constexpr int MASTERNODE_SELF_CHECK_INTERVAL_BLOCKS{24};
/** Do not repeat the self-check warning in the log more often than this. */
static constexpr int64_t MASTERNODE_SELF_CHECK_LOG_INTERVAL{3600};

class CActiveMasternodeManager
{
private:
    CConnman& m_connman;
    CDeterministicMNManager& m_dmnman;
    const CBLSPublicKey m_operator_pk;
    const CBLSSecretKey m_operator_sk;
    const bool m_selfcheck_strict;

private:
    enum class MasternodeState {
        WAITING_FOR_PROTX,
        POSE_BANNED,
        REMOVED,
        OPERATOR_KEY_CHANGED,
        PROTX_IP_CHANGED,
        READY,
        SOME_ERROR,
    };

    /** Outcome of the most recent self-connect probe. Never affects consensus or the MN list. */
    enum class SelfCheckResult {
        SKIPPED,    // chain does not require a routable external IP (regtest)
        OK,         // we reached our own advertised address
        FAILED,     // probe failed and -masternodeselfcheck=1 turned that into a hard error
        STRICT_OFF, // probe failed but we keep running because -masternodeselfcheck=0
    };

    mutable SharedMutex cs;
    COutPoint m_outpoint GUARDED_BY(cs);
    CService m_service GUARDED_BY(cs);
    MasternodeState m_state GUARDED_BY(cs){MasternodeState::WAITING_FOR_PROTX};
    std::string m_error GUARDED_BY(cs);
    // Non-fatal operator-facing diagnostic. Unlike m_error it does not keep us out of DKG rounds.
    std::string m_warning GUARDED_BY(cs);
    int64_t m_warning_log_time GUARDED_BY(cs){0};
    SelfCheckResult m_selfcheck GUARDED_BY(cs){SelfCheckResult::SKIPPED};
    // Height of the last probe, -1 when we have not probed since becoming READY.
    int m_selfcheck_height GUARDED_BY(cs){-1};
    uint256 m_protx_hash GUARDED_BY(cs);

public:
    CActiveMasternodeManager() = delete;
    CActiveMasternodeManager(const CActiveMasternodeManager&) = delete;
    CActiveMasternodeManager& operator=(const CActiveMasternodeManager&) = delete;
    explicit CActiveMasternodeManager(CConnman& connman, CDeterministicMNManager& dmnman, const CBLSSecretKey& sk,
                                      bool selfcheck_strict);
    ~CActiveMasternodeManager();

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void Init(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs) { LOCK(cs); InitInternal(pindex); };

    std::string GetStateString() const;
    std::string GetStatus() const;
    [[nodiscard]] std::string GetWarning() const { READ_LOCK(cs); return m_warning; }
    [[nodiscard]] std::string GetSelfCheckString() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    static bool IsValidNetAddr(const CService& addrIn);

    template <template <typename> class EncryptedObj, typename Obj>
    [[nodiscard]] bool Decrypt(const EncryptedObj<Obj>& obj, size_t idx, Obj& ret_obj, int version) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs);
    [[nodiscard]] CBLSSignature Sign(const uint256& hash, const bool is_legacy) const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    [[nodiscard]] std::vector<uint8_t> SignBasic(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    [[nodiscard]] COutPoint GetOutPoint() const { READ_LOCK(cs); return m_outpoint; }
    [[nodiscard]] uint256 GetProTxHash() const { READ_LOCK(cs); return m_protx_hash; }
    [[nodiscard]] CService GetService() const { READ_LOCK(cs); return m_service; }
    [[nodiscard]] CBLSPublicKey GetPubKey() const;

private:
    void InitInternal(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs);
    //! Probe our own advertised address. Returns false only when strict mode turns a failure fatal.
    bool RunSelfCheck() EXCLUSIVE_LOCKS_REQUIRED(cs);
    //! Re-probe if enough blocks have passed since the last probe. Never changes m_state.
    void MaybeRunSelfCheck(int nHeight) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool GetLocalAddress(CService& addrRet) EXCLUSIVE_LOCKS_REQUIRED(cs);
};

#endif // BITCOIN_ACTIVE_MASTERNODE_H
