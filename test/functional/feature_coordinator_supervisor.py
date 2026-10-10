#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the coordinator supervisor (-coordinator, getcoordinatorinfo).

kerrigand runs kerrigan-coordinator while the node is an enabled BroodNode.
This test swaps in a fake coordinator (-coordinatorpath) that records how it
was started, and checks how the node starts, restarts and stops it. The
BroodNode case itself is covered by feature_coordinator_broodnode.py.
"""

import json
import os
import stat
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port

FAKE = """#!/usr/bin/env python3
import json, os, signal, sys, time
state = {state!r}
pid = os.getpid()
env = {{k: v for k, v in os.environ.items() if k.startswith("KRGN_")}}
with open(os.path.join(state, "start-%d.json" % pid), "w") as f:
    json.dump({{"pid": pid, "argv": sys.argv[1:], "env": env}}, f)
mode_file = os.path.join(state, "mode")
if os.path.exists(mode_file) and open(mode_file).read().strip() == "crash":
    sys.exit(3)
def stop(*_):
    open(os.path.join(state, "stopped-%d" % pid), "w").close()
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
while True:
    time.sleep(0.2)
"""


class CoordinatorSupervisorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_fake(self):
        self.state = os.path.join(self.options.tmpdir, "fake_coordinator")
        os.makedirs(self.state, exist_ok=True)
        self.fake = os.path.join(self.options.tmpdir, "fake-coordinator")
        with open(self.fake, "w") as f:
            f.write(FAKE.format(state=self.state))
        os.chmod(self.fake, os.stat(self.fake).st_mode | stat.S_IXUSR)

    def starts(self):
        out = []
        for name in sorted(os.listdir(self.state)):
            if name.startswith("start-"):
                with open(os.path.join(self.state, name)) as f:
                    out.append(json.load(f))
        return out

    def set_mode(self, mode):
        with open(os.path.join(self.state, "mode"), "w") as f:
            f.write(mode)

    def tick(self):
        """Run the supervisor's scheduled check now."""
        self.nodes[0].mockscheduler(16)

    def wait_blockchain_synced(self):
        node = self.nodes[0]
        def synced():
            if node.mnsync("status")["IsBlockchainSynced"]:
                return True
            node.mnsync("next")
            return False
        self.wait_until(synced)

    def log_fds_in_node(self):
        """kerrigand's open descriptors on the coordinator log (must stay 0)."""
        pid = self.nodes[0].process.pid
        count = 0
        for fd in os.listdir("/proc/%d/fd" % pid):
            try:
                if os.readlink("/proc/%d/fd/%s" % (pid, fd)).endswith("coordinator.log"):
                    count += 1
            except OSError:
                pass
        return count

    def run_test(self):
        node = self.nodes[0]
        self.setup_fake()
        self.generate(node, 1)
        self.wait_blockchain_synced()

        self.log.info("auto (the default) does nothing on a node that is not a BroodNode")
        self.tick()
        info = node.getcoordinatorinfo()
        assert_equal(info["mode"], "auto")
        assert_equal(info["running"], False)
        assert_equal(info["status"], "not an active masternode")

        self.log.info("an invalid -coordinator value is an init error")
        self.stop_node(0)
        node.assert_start_raises_init_error(["-coordinator=yes"], "Error: Invalid -coordinator value 'yes'. Use auto, 1 or 0.")

        self.log.info("-coordinator=0 never starts it")
        self.start_node(0, ["-coordinator=0", "-coordinatorpath=" + self.fake])
        self.wait_blockchain_synced()
        self.tick()
        assert_equal(node.getcoordinatorinfo()["status"], "disabled (-coordinator=0)")
        assert_equal(self.starts(), [])

        self.log.info("-coordinator=1 starts it with the node's RPC cookie, never a password on the command line")
        self.restart_node(0, ["-coordinator=1", "-coordinatorpath=" + self.fake])
        self.wait_blockchain_synced()
        self.tick()
        self.wait_until(lambda: len(self.starts()) == 1)
        info = node.getcoordinatorinfo()
        assert_equal(info["running"], True)
        assert_equal(info["status"], "running")
        start = self.starts()[0]
        assert_equal(info["pid"], start["pid"])
        argv = start["argv"]
        def arg(name):
            return argv[argv.index(name) + 1]
        assert_equal(arg("--rpc-url"), "http://127.0.0.1:%d" % rpc_port(node.index))
        assert arg("--rpc-url").startswith("http://127.0.0.1:")
        datadir = os.path.join(node.datadir, self.chain)
        assert_equal(arg("--rpc-cookie-file"), os.path.join(datadir, ".cookie"))
        assert_equal(arg("--db-path"), os.path.join(datadir, "coordinator", "coordinator.db"))
        assert_equal(arg("--bind"), "0.0.0.0:7122")
        assert "--exit-with-parent" in argv
        assert "--directory-allow-private" in argv
        assert "--public-url" not in argv  # not a BroodNode: no registered address to advertise
        assert "--iroh-port" not in argv
        assert_equal(start["env"], {})
        assert_equal(self.log_fds_in_node(), 0)

        self.log.info("a coordinator that dies is restarted after a backoff")
        os.kill(start["pid"], 9)
        self.wait_until(lambda: (self.tick() or True) and node.getcoordinatorinfo()["restarts"] == 1)
        info = node.getcoordinatorinfo()
        assert_equal(info["running"], False)
        assert_equal(info["last_exit_code"], 9)
        assert info["status"].startswith("restarting in "), info["status"]
        time.sleep(11)  # the first backoff is 10 s of real time
        self.tick()
        self.wait_until(lambda: len(self.starts()) == 2)
        assert_equal(node.getcoordinatorinfo()["running"], True)
        assert_equal(self.log_fds_in_node(), 0)

        self.log.info("stopping the node stops the coordinator")
        pid = node.getcoordinatorinfo()["pid"]
        self.stop_node(0)
        assert os.path.exists(os.path.join(self.state, "stopped-%d" % pid))

        self.log.info("with -rpcpassword the credentials go in the environment, not on the command line")
        with open(os.path.join(node.datadir, "kerrigan.conf"), "a") as f:
            f.write("rpcuser=coordtest\nrpcpassword=coordsecret\n")
        self.start_node(0, ["-coordinator=1", "-coordinatorpath=" + self.fake])
        self.wait_blockchain_synced()
        self.tick()
        self.wait_until(lambda: len(self.starts()) == 3)
        start = [s for s in self.starts() if s["pid"] == node.getcoordinatorinfo()["pid"]][0]
        assert_equal(start["env"], {"KRGN_RPC_USER": "coordtest", "KRGN_RPC_PASS": "coordsecret"})
        assert "--rpc-cookie-file" not in start["argv"]
        assert "coordsecret" not in " ".join(start["argv"])

        self.log.info("a coordinator that keeps crashing does not take the node down")
        self.set_mode("crash")
        os.kill(start["pid"], 9)
        self.wait_until(lambda: (self.tick() or True) and node.getcoordinatorinfo()["restarts"] == 1)
        time.sleep(11)
        self.tick()
        self.wait_until(lambda: (self.tick() or True) and node.getcoordinatorinfo()["restarts"] == 2)
        assert_equal(node.getcoordinatorinfo()["last_exit_code"], 3)
        assert_equal(node.getblockcount(), 1)

        self.log.info("a missing executable is reported, and the node keeps running")
        self.restart_node(0, ["-coordinator=1", "-coordinatorpath=/nonexistent/kerrigan-coordinator"])
        self.wait_blockchain_synced()
        self.tick()
        self.wait_until(lambda: node.getcoordinatorinfo()["status"].startswith("could not start /nonexistent/kerrigan-coordinator"))
        assert_equal(node.getcoordinatorinfo()["running"], False)


if __name__ == '__main__':
    CoordinatorSupervisorTest().main()
