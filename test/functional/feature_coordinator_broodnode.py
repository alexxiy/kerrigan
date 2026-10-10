#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a BroodNode runs the coordinator at its registered address.

With the default -coordinator=auto, kerrigand starts the coordinator once the
node is an enabled BroodNode, at the platform HTTPS address in its ProTx, and
restarts it on the new address when a ProUpServTx changes that address. A
fake coordinator (-coordinatorpath) records how it was started; the
supervisor's other behaviour is covered by feature_coordinator_supervisor.py.
"""

import json
import os
import stat

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal, p2p_port

FAKE = """#!/usr/bin/env python3
import json, os, signal, sys, time
state = {state!r}
pid = os.getpid()
with open(os.path.join(state, "start-%d.json" % pid), "w") as f:
    json.dump({{"pid": pid, "argv": sys.argv[1:]}}, f)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
while True:
    time.sleep(0.2)
"""


class CoordinatorBroodNodeTest(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_kerrigan_test_params(2, 0, evo_count=1)

    def setup_fake(self):
        self.state = os.path.join(self.options.tmpdir, "fake_coordinator")
        os.makedirs(self.state, exist_ok=True)
        self.fake = os.path.join(self.options.tmpdir, "fake-coordinator")
        with open(self.fake, "w") as f:
            f.write(FAKE.format(state=self.state))
        os.chmod(self.fake, os.stat(self.fake).st_mode | stat.S_IXUSR)
        # Every node, including the BroodNode added below, gets the fake.
        self.extra_args = [args + ["-coordinatorpath=" + self.fake] for args in self.extra_args]

    def setup_nodes(self):
        self.setup_fake()
        super().setup_nodes()

    def starts(self):
        out = []
        for name in sorted(os.listdir(self.state)):
            if name.startswith("start-"):
                with open(os.path.join(self.state, name)) as f:
                    out.append(json.load(f))
        return out

    def started_argv(self, node):
        """argv of the coordinator now running for `node`, once it has recorded its start."""
        pid = node.getcoordinatorinfo()["pid"]
        self.wait_until(lambda: any(s["pid"] == pid for s in self.starts()))
        return [s for s in self.starts() if s["pid"] == pid][0]["argv"]

    def running_on(self, node, url):
        node.mockscheduler(16)
        info = node.getcoordinatorinfo()
        return info["running"] and info.get("public_url") == url

    def run_test(self):
        self.log.info("Register a BroodNode")
        brood = self.dynamically_add_masternode(evo=True)
        node = self.nodes[brood.nodeIdx]
        listed = self.nodes[0].protx("info", brood.proTxHash)
        assert_equal(listed["type"], "BroodNode")
        https = listed["state"]["addresses"]["platform_https"][0]
        assert_equal(https, "127.0.0.1:%d" % (p2p_port(brood.nodeIdx) + 102))

        self.log.info("It runs the coordinator at its registered platform HTTPS address")
        self.wait_until(lambda: self.running_on(node, "http://" + https))
        argv = self.started_argv(node)
        assert_equal(argv[argv.index("--bind") + 1], "0.0.0.0:%s" % https.split(":")[1])
        assert_equal(argv[argv.index("--public-url") + 1], "http://" + https)
        p2p = listed["state"]["addresses"]["platform_p2p"][0]
        assert_equal(argv[argv.index("--iroh-port") + 1], p2p.split(":")[1])

        self.log.info("The non-masternode node does not")
        self.nodes[0].mockscheduler(16)
        assert_equal(self.nodes[0].getcoordinatorinfo()["running"], False)

        self.log.info("A ProUpServTx moves it to the new address")
        self.dynamically_evo_update_service(brood, rnd=31000)
        self.sync_blocks()
        new_https = self.nodes[0].protx("info", brood.proTxHash)["state"]["addresses"]["platform_https"][0]
        assert new_https != https
        self.wait_until(lambda: self.running_on(node, "http://" + new_https))
        assert_equal(self.started_argv(node)[self.started_argv(node).index("--public-url") + 1], "http://" + new_https)


if __name__ == '__main__':
    CoordinatorBroodNodeTest().main()
