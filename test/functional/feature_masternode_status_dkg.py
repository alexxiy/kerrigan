#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_masternode_status_dkg.py

v1.3.3 masternode operator hardening.

Covers three operator-facing changes that carry no consensus weight:

  1. `masternode status` reports a "warning" string. It is empty while the
     self-check succeeds; a failed self-connect no longer forces the node out
     of every DKG round (see -masternodeselfcheck).
  2. `masternode status` reports a "lastDkg" object describing the newest mined
     commitment of a quorum this node was a member of, and the node logs one
     line per mined commitment saying whether it ended up valid or excluded.
  3. LLMQ types that can never form on this network (llmq_test_dip0024 needs 4
     members, the test chain has 3) no longer print "quorum initialization
     failed" on every round at default log level; they print one summary line
     per type per day instead.
"""

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal


class MasternodeStatusDkgTest(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # Node 1 is the first masternode. Run it with the pre-v1.3.3 strict self-check to prove the
        # option parses and that a reachable masternode is unaffected by it.
        extra_args = [[] for _ in range(4)]
        extra_args[1] = ["-masternodeselfcheck=1"]
        self.set_kerrigan_test_params(4, 3, extra_args)

    def read_log(self, node):
        with open(node.debug_log_path, encoding='utf-8') as f:
            return f.read()

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("masternode status exposes warning and lastDkg")
        for mn in self.mninfo:
            status = mn.get_node(self).masternode("status")
            assert "warning" in status, "masternode status is missing the warning field"
            assert isinstance(status["warning"], str)
            # The regtest self-connect targets our own listening port, so it succeeds.
            assert_equal(status["warning"], "")
            assert "lastDkg" in status, "masternode status is missing the lastDkg field"
            assert isinstance(status["lastDkg"], dict)

        # regtest does not require a routable external IP, so the self-connect check is skipped
        # there and strict mode cannot be made to fail; assert only that it does not regress.
        assert_equal(self.nodes[1].masternode("status")["warning"], "")

        self.log.info("lastDkg reports membership and validity of the mined commitment")
        self.mine_quorum()

        for mn in self.mninfo:
            node = mn.get_node(self)
            status = node.masternode("status")
            assert_equal(status["state"], "READY")
            last_dkg = status["lastDkg"]
            assert last_dkg, "lastDkg is empty after mine_quorum()"
            assert_equal(last_dkg["llmqType"], "llmq_test")
            assert_equal(last_dkg["member"], True)
            assert_equal(last_dkg["valid"], True)
            assert last_dkg["height"] > 0
            assert len(last_dkg["quorumHash"]) == 64

            log = self.read_log(node)
            expected = "DKG: masternode %s was valid in quorum llmq_test" % mn.proTxHash
            assert expected in log, "missing per-commitment outcome line: %s" % expected

        # A second round gives every rotating llmq type a chance to attempt a DKG.
        self.mine_quorum()

        self.log.info("unformable llmq types are not logged every round")
        for mn in self.mninfo:
            log = self.read_log(mn.get_node(self))
            # The per-round line survives, but only under the llmq-dkg category. Lines without the
            # category tag are the ones an operator sees at the default log level.
            spam = [line for line in log.splitlines()
                    if "quorum initialization failed for llmq_test_dip0024" in line
                    and "[llmq-dkg]" not in line]
            assert_equal(spam, [])
            assert "llmq type llmq_test_dip0024 cannot form:" in log, \
                "missing the once-per-day summary for llmq_test_dip0024"


if __name__ == '__main__':
    MasternodeStatusDkgTest().main()
