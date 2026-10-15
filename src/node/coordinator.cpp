// Copyright (c) 2026 The Kerrigan developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <node/coordinator.h>

#include <active/context.h>
#include <active/masternode.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/netinfo.h>
#include <logging.h>
#include <masternode/sync.h>
#include <netaddress.h>
#include <node/context.h>
#include <scheduler.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/system.h>

#ifdef ENABLE_EXTERNAL_SIGNER
#include <util/subprocess.hpp>
#endif

#include <csignal>
#include <thread>

#ifndef WIN32
#include <unistd.h>
#endif

namespace node {

namespace {
/** File name of the coordinator executable shipped next to kerrigand. */
constexpr const char* COORDINATOR_BINARY = "kerrigan-coordinator";
/** How often the supervisor looks at node state. */
constexpr std::chrono::seconds SUPERVISE_INTERVAL{15};
/** Restart backoff after the coordinator exits: doubles from the first value up to the second. */
constexpr std::chrono::seconds BACKOFF_MIN{10};
constexpr std::chrono::seconds BACKOFF_MAX{600};
/** A run this long counts as healthy and resets the backoff. */
constexpr std::chrono::seconds HEALTHY_RUN{600};
/** Time the coordinator gets to shut down cleanly before it is killed (it caps its own shutdown at 3 s). */
constexpr std::chrono::seconds STOP_GRACE{5};
/** The coordinator log is rotated to coordinator.log.1 once it passes this size, checked on every supervise pass. */
constexpr uintmax_t LOG_ROTATE_BYTES{10 * 1024 * 1024};
/** Port used when the node has no registered coordinator address (-coordinator=1 on a non-BroodNode). */
constexpr uint16_t DEFAULT_COORDINATOR_PORT{7122};

/**
 * An address a BroodNode registered for @p purpose: its platform_https entry
 * (the coordinator's REST API) or platform_p2p entry (the UDP port drones
 * reach it on), else its service IP at platformHTTPPort / platformP2PPort.
 */
std::optional<CService> RegisteredEndpoint(const CDeterministicMN& dmn, NetInfoPurpose purpose)
{
    const auto& state = *dmn.pdmnState;
    if (!state.netInfo) return std::nullopt;
    if (state.netInfo->CanStorePlatform()) {
        for (const auto& entry : state.netInfo->GetEntries(purpose)) {
            if (const auto service = entry.GetAddrPort()) return *service;
        }
        return std::nullopt;
    }
    const uint16_t port = purpose == NetInfoPurpose::PLATFORM_HTTPS ? state.platformHTTPPort : state.platformP2PPort;
    if (port == 0) return std::nullopt;
    return CService(state.netInfo->GetPrimary(), port);
}

/**
 * kerrigan-coordinator next to the running kerrigand, or an empty path when
 * that cannot be determined. Never a bare name: a PATH lookup could run
 * whatever binary of that name comes first.
 */
fs::path DefaultBinary()
{
#ifdef __linux__
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return fs::PathFromString(std::string(buf, len)).parent_path() / COORDINATOR_BINARY;
    }
#endif
    return {};
}

/** A ZMQ endpoint the coordinator can connect to: a wildcard bind becomes loopback. */
std::string ConnectableZmq(std::string endpoint)
{
    for (const std::string wildcard : {"tcp://0.0.0.0:", "tcp://*:"}) {
        if (endpoint.rfind(wildcard, 0) == 0) return "tcp://127.0.0.1:" + endpoint.substr(wildcard.size());
    }
    return endpoint;
}

/**
 * Keep the coordinator log under LOG_ROTATE_BYTES: copy it to
 * coordinator.log.1 and truncate it. Truncating, not renaming, works while the
 * coordinator runs: it writes with O_APPEND, so it carries on at the new end.
 */
void RotateLog(const fs::path& log)
{
    try {
        if (fs::exists(log) && fs::file_size(log) > LOG_ROTATE_BYTES) {
            fs::path old = log;
            old += ".1";
            fs::copy_file(log, old, fs::copy_options::overwrite_existing);
            fs::resize_file(log, 0);
        }
    } catch (const fs::filesystem_error& e) {
        LogPrintf("Coordinator: could not rotate %s: %s\n", fs::PathToString(log), e.what());
    }
}
} // namespace

std::optional<CoordinatorMode> ParseCoordinatorMode(const std::string& value)
{
    if (value == "auto") return CoordinatorMode::WHEN_BROODNODE;
    if (value == "1") return CoordinatorMode::ANY_NODE;
    if (value == "0") return CoordinatorMode::DISABLED;
    return std::nullopt;
}

struct CoordinatorSupervisor::Child {
#ifdef ENABLE_EXTERNAL_SIGNER
    std::unique_ptr<subprocess::Popen> proc;
#endif
    int pid{0};
};

CoordinatorSupervisor::CoordinatorSupervisor(NodeContext& node, const ArgsManager& args) :
    m_node{node},
    m_args{args},
    m_mode{ParseCoordinatorMode(args.GetArg("-coordinator", "auto")).value_or(CoordinatorMode::WHEN_BROODNODE)}
{
}

CoordinatorSupervisor::~CoordinatorSupervisor()
{
    Stop();
}

bool CoordinatorSupervisor::Supported()
{
#if defined(ENABLE_EXTERNAL_SIGNER) && !defined(WIN32)
    return true;
#else
    return false;
#endif
}

std::optional<CoordinatorSupervisor::LaunchPlan> CoordinatorSupervisor::Plan(std::string& reason) const
{
    if (m_mode == CoordinatorMode::DISABLED) {
        reason = "disabled (-coordinator=0)";
        return std::nullopt;
    }
    if (!Supported()) {
        reason = "not supported on this platform";
        return std::nullopt;
    }
    if (!m_args.GetBoolArg("-server", false)) {
        reason = "the RPC server is off (-server=0); the coordinator reads the node over RPC";
        return std::nullopt;
    }
    if (!m_node.mn_sync || !m_node.mn_sync->IsBlockchainSynced()) {
        reason = "waiting for the chain to sync";
        return std::nullopt;
    }

    // This node's registered coordinator address, when it is a masternode.
    std::optional<CService> endpoint;
    std::optional<CService> p2p_endpoint;
    const uint256 protx = (m_node.active_ctx && m_node.active_ctx->nodeman) ? m_node.active_ctx->nodeman->GetProTxHash() : uint256{};
    CDeterministicMNCPtr dmn;
    if (!protx.IsNull() && m_node.dmnman) dmn = m_node.dmnman->GetListAtChainTip().GetMN(protx);
    if (m_mode == CoordinatorMode::WHEN_BROODNODE) {
        if (!dmn) {
            reason = "not an active masternode";
            return std::nullopt;
        }
        if (dmn->nType != MnType::Evo) {
            reason = "not a BroodNode";
            return std::nullopt;
        }
        if (dmn->pdmnState->IsBanned()) {
            reason = "this BroodNode is PoSe-banned";
            return std::nullopt;
        }
    }
    if (dmn && dmn->nType == MnType::Evo) {
        endpoint = RegisteredEndpoint(*dmn, NetInfoPurpose::PLATFORM_HTTPS);
        p2p_endpoint = RegisteredEndpoint(*dmn, NetInfoPurpose::PLATFORM_P2P);
    }
    if (m_mode == CoordinatorMode::WHEN_BROODNODE && !endpoint) {
        reason = "this BroodNode registered no platform HTTPS address";
        return std::nullopt;
    }

    LaunchPlan plan;
    plan.binary = m_args.IsArgSet("-coordinatorpath") ? m_args.GetPathArg("-coordinatorpath") : DefaultBinary();
    if (plan.binary.empty() || !plan.binary.is_absolute()) {
        reason = "set -coordinatorpath to the absolute path of kerrigan-coordinator";
        return std::nullopt;
    }

    const fs::path dir = m_args.GetDataDirNet() / "coordinator";
    plan.log_file = dir / "coordinator.log";

    const uint16_t port = endpoint ? endpoint->GetPort() : DEFAULT_COORDINATOR_PORT;
    std::string bind = m_args.GetArg("-coordinatorbind", "");
    if (bind.empty()) bind = strprintf("%s:%d", (endpoint && endpoint->IsIPv6()) ? "[::]" : "0.0.0.0", port);
    if (endpoint) plan.public_url = "http://" + endpoint->ToStringAddrPort();

    plan.args = {
        "--rpc-url", strprintf("http://127.0.0.1:%d", m_args.GetIntArg("-rpcport", BaseParams().RPCPort())),
        "--db-path", fs::PathToString(dir / "coordinator.db"),
        "--bind", bind,
        "--exit-with-parent",
    };
    // The RPC password never goes on the command line, where other users can read it.
    const std::string rpc_password = m_args.GetArg("-rpcpassword", "");
    if (!rpc_password.empty()) {
        plan.env["KRGN_RPC_USER"] = m_args.GetArg("-rpcuser", "");
        plan.env["KRGN_RPC_PASS"] = rpc_password;
    } else {
        plan.args.insert(plan.args.end(), {"--rpc-cookie-file", fs::PathToString(AbsPathForConfigVal(m_args.GetPathArg("-rpccookiefile", ".cookie")))});
    }
    if (!plan.public_url.empty()) plan.args.insert(plan.args.end(), {"--public-url", plan.public_url});
    // Drones connect over Iroh (UDP) on the registered platform P2P port, so operators know what to open.
    if (p2p_endpoint) plan.args.insert(plan.args.end(), {"--iroh-port", strprintf("%d", p2p_endpoint->GetPort())});
    const auto zmq = m_args.GetArgs("-zmqpubhashblock");
    if (!zmq.empty()) plan.args.insert(plan.args.end(), {"--zmq-url", ConnectableZmq(zmq.front())});
    // Regtest masternodes run on loopback, which the directory skips everywhere else.
    if (Params().NetworkIDString() == CBaseChainParams::REGTEST) plan.args.emplace_back("--directory-allow-private");
    return plan;
}

void CoordinatorSupervisor::StartLocked(const LaunchPlan& plan)
{
    AssertLockHeld(m_mutex);
#ifdef ENABLE_EXTERNAL_SIGNER
    try {
        fs::create_directories(plan.log_file.parent_path());
    } catch (const fs::filesystem_error& e) {
        m_status = strprintf("could not create %s: %s", fs::PathToString(plan.log_file.parent_path()), e.what());
        return;
    }
    RotateLog(plan.log_file);

    std::vector<std::string> cmd{fs::PathToString(plan.binary)};
    cmd.insert(cmd.end(), plan.args.begin(), plan.args.end());
    subprocess::env_map_t env(plan.env.begin(), plan.env.end());
    const std::string log = fs::PathToString(plan.log_file);
    try {
        auto proc = std::make_unique<subprocess::Popen>(cmd,
                                                        subprocess::input{"/dev/null"},
                                                        subprocess::output{log.c_str()},
                                                        subprocess::error{log.c_str()},
                                                        subprocess::environment{env},
                                                        subprocess::close_fds{true});
        m_child = std::make_unique<Child>();
        m_child->pid = proc->pid();
        m_child->proc = std::move(proc);
        m_started_at = std::chrono::steady_clock::now();
        m_public_url = plan.public_url;
        m_running_args = plan.args;
        m_log_file = plan.log_file;
        m_status = "running";
        LogPrintf("Coordinator: started %s (pid %d)%s, log %s\n", fs::PathToString(plan.binary), m_child->pid,
                  plan.public_url.empty() ? "" : " serving " + plan.public_url, log);
    } catch (const std::exception& e) {
        m_child.reset();
        m_backoff = std::clamp(m_backoff * 2, BACKOFF_MIN, BACKOFF_MAX);
        m_next_start = std::chrono::steady_clock::now() + m_backoff;
        m_status = strprintf("could not start %s: %s", fs::PathToString(plan.binary), e.what());
        LogPrintf("Coordinator: %s; retrying in %ds\n", m_status, m_backoff.count());
    }
#endif
}

void CoordinatorSupervisor::StopLocked(const std::string& why)
{
    AssertLockHeld(m_mutex);
    if (!m_child) return;
#ifdef ENABLE_EXTERNAL_SIGNER
    LogPrintf("Coordinator: stopping (pid %d): %s\n", m_child->pid, why);
    try {
        m_child->proc->kill(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + STOP_GRACE;
        int rc = m_child->proc->poll();
        while (rc == -1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            rc = m_child->proc->poll();
        }
        if (rc == -1) {
            LogPrintf("Coordinator: did not stop within %ds; killing it\n", STOP_GRACE.count());
            m_child->proc->kill(SIGKILL);
            m_child->proc->wait();
        }
    } catch (const std::exception& e) {
        LogPrintf("Coordinator: error while stopping: %s\n", e.what());
    }
#endif
    m_child.reset();
    m_public_url.clear();
    m_running_args.clear();
}

void CoordinatorSupervisor::Supervise()
{
    std::string reason;
    const auto plan = Plan(reason);

    LOCK(m_mutex);
    if (m_stopped) return;
    const auto now = std::chrono::steady_clock::now();

#ifdef ENABLE_EXTERNAL_SIGNER
    // Reap a coordinator that exited on its own.
    if (m_child) {
        int rc{-1};
        try {
            rc = m_child->proc->poll();
        } catch (const std::exception&) {
            rc = 255;
        }
        if (rc != -1) {
            m_last_exit = rc;
            ++m_restarts;
            const bool healthy = now - m_started_at >= HEALTHY_RUN;
            m_backoff = healthy ? BACKOFF_MIN : std::clamp(m_backoff * 2, BACKOFF_MIN, BACKOFF_MAX);
            m_next_start = now + m_backoff;
            LogPrintf("Coordinator: exited with code %d (pid %d); restarting in %ds\n", rc, m_child->pid, m_backoff.count());
            m_child.reset();
            m_public_url.clear();
        }
    }
#endif

    if (!plan) {
        StopLocked(reason);
        m_status = reason;
        return;
    }
    if (m_child) {
        // Anything it was started with changed (a ProUpServTx moving either
        // registered port or address, ZMQ, bind): restart with the new plan.
        if (plan->args != m_running_args) {
            StopLocked("its settings changed" + (plan->public_url.empty() ? std::string{} : ", serving " + plan->public_url));
        } else {
            RotateLog(m_log_file);
            return;
        }
    }
    if (now < m_next_start) {
        m_status = strprintf("restarting in %ds", std::chrono::duration_cast<std::chrono::seconds>(m_next_start - now).count());
        return;
    }
    StartLocked(*plan);
}

void CoordinatorSupervisor::Schedule(CScheduler& scheduler)
{
    scheduler.scheduleEvery([this] { Supervise(); }, SUPERVISE_INTERVAL);
}

void CoordinatorSupervisor::Stop()
{
    LOCK(m_mutex);
    m_stopped = true;
    StopLocked("node shutting down");
    m_status = "stopped";
}

UniValue CoordinatorSupervisor::ToJson() const
{
    LOCK(m_mutex);
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("supported", Supported());
    obj.pushKV("mode", m_mode == CoordinatorMode::WHEN_BROODNODE ? "auto" : m_mode == CoordinatorMode::ANY_NODE ? "always" : "off");
    obj.pushKV("running", m_child != nullptr);
    if (m_child) obj.pushKV("pid", m_child->pid);
    obj.pushKV("status", m_status);
    if (!m_public_url.empty()) obj.pushKV("public_url", m_public_url);
    obj.pushKV("restarts", m_restarts);
    if (m_last_exit) obj.pushKV("last_exit_code", *m_last_exit);
    return obj;
}

} // namespace node
