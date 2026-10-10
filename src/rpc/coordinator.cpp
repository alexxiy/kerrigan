// Copyright (c) 2026 The Kerrigan developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/context.h>
#include <node/coordinator.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <univalue.h>

static RPCHelpMan getcoordinatorinfo()
{
    return RPCHelpMan{"getcoordinatorinfo",
        "\nState of the AI swarm coordinator this node runs.\n"
        "Serving a coordinator is part of the BroodNode role: kerrigand starts kerrigan-coordinator\n"
        "while this node is an enabled BroodNode on a synced chain, at the platform HTTPS address\n"
        "it registered, and restarts it if it exits (see -coordinator).\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "supported", "Whether this build can run a coordinator"},
                {RPCResult::Type::STR, "mode", "auto, always or off (-coordinator)"},
                {RPCResult::Type::BOOL, "running", "Whether the coordinator is running"},
                {RPCResult::Type::NUM, "pid", /*optional=*/true, "Its process id"},
                {RPCResult::Type::STR, "status", "What the supervisor is doing, or why the coordinator is not running"},
                {RPCResult::Type::STR, "public_url", /*optional=*/true, "The address it serves on"},
                {RPCResult::Type::NUM, "restarts", "How many times it exited and was restarted"},
                {RPCResult::Type::NUM, "last_exit_code", /*optional=*/true, "Exit code of the last exit"},
            }},
        RPCExamples{
            HelpExampleCli("getcoordinatorinfo", "")
    + HelpExampleRpc("getcoordinatorinfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const node::NodeContext& node = EnsureAnyNodeContext(request.context);
            if (!node.coordinator) throw JSONRPCError(RPC_INTERNAL_ERROR, "The coordinator supervisor is not running");
            return node.coordinator->ToJson();
        },
    };
}

void RegisterCoordinatorRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"evo", &getcoordinatorinfo},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
