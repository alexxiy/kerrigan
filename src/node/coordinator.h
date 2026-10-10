// Copyright (c) 2026 The Kerrigan developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_COORDINATOR_H
#define BITCOIN_NODE_COORDINATOR_H

#include <sync.h>
#include <fs.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ArgsManager;
class CScheduler;
class UniValue;

namespace node {
struct NodeContext;

/** -coordinator values. */
enum class CoordinatorMode {
    WHEN_BROODNODE, //!< run while this node is an active BroodNode (the default, -coordinator=auto)
    ANY_NODE,       //!< run on any node: testing, or a regular masternode that volunteers (-coordinator=1)
    DISABLED,       //!< never run (-coordinator=0)
};

/**
 * Runs the AI swarm coordinator (kerrigan-coordinator) next to the node.
 *
 * Serving a coordinator is part of the BroodNode role: drones and inference
 * clients find coordinators through the BroodNode list, at the platform_https
 * address each BroodNode registered. The supervisor checks the node's own
 * masternode state on the scheduler, starts the coordinator while this node is
 * an enabled BroodNode on a synced chain, restarts it with a backoff if it
 * exits, and stops it when the node stops being one or shuts down.
 *
 * The coordinator is a separate process, so a fault in it cannot take the
 * node down. It reads the node over local RPC, and exits on its own if the
 * node dies without stopping it (--exit-with-parent).
 */
class CoordinatorSupervisor
{
public:
    /** How the coordinator is started: everything Supervise() decides from node state. */
    struct LaunchPlan {
        fs::path binary;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        fs::path log_file;
        std::string public_url;
    };

    CoordinatorSupervisor(NodeContext& node, const ArgsManager& args);
    ~CoordinatorSupervisor();

    /** Check node state and start, restart or stop the coordinator. Called on the scheduler. */
    void Supervise() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Schedule(CScheduler& scheduler);
    /** Stop the coordinator and do not start it again. */
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** State for the getcoordinatorinfo RPC. */
    UniValue ToJson() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether this build can start a coordinator at all. */
    static bool Supported();

private:
    struct Child;

    /** Why the coordinator should run now, or nullopt with the reason it should not. */
    std::optional<LaunchPlan> Plan(std::string& reason) const;
    void StartLocked(const LaunchPlan& plan) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void StopLocked(const std::string& why) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    NodeContext& m_node;
    const ArgsManager& m_args;
    const CoordinatorMode m_mode;

    mutable Mutex m_mutex;
    std::unique_ptr<Child> m_child GUARDED_BY(m_mutex);
    bool m_stopped GUARDED_BY(m_mutex){false};
    std::string m_status GUARDED_BY(m_mutex){"not checked yet"};
    std::string m_public_url GUARDED_BY(m_mutex);
    int m_restarts GUARDED_BY(m_mutex){0};
    std::optional<int> m_last_exit GUARDED_BY(m_mutex);
    std::chrono::seconds m_backoff GUARDED_BY(m_mutex){0};
    std::chrono::steady_clock::time_point m_next_start GUARDED_BY(m_mutex);
    std::chrono::steady_clock::time_point m_started_at GUARDED_BY(m_mutex);
};

/** Parse -coordinator. Returns nullopt for an invalid value. */
std::optional<CoordinatorMode> ParseCoordinatorMode(const std::string& value);

} // namespace node

#endif // BITCOIN_NODE_COORDINATOR_H
