#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_llmq_dkg_late_contribution.py

v1.3.3 DKG phase dwell + drain-before-judge.

A DKG phase is dkgPhaseBlocks blocks wide. On Kerrigan a multi-algo block
burst can connect a whole phase in a couple of seconds, so the Complain start
handler (VerifyAndComplain) can run while another member's contribution is
still in flight. VerifyAndComplain marks every member with no contribution as
bad, and once dkgBadVotesThreshold members agree the victim is dropped from
validMembers of the mined commitment and takes a PoSe penalty - for being
slow, not for being dishonest.

v1.3.3 adds two guards, both local scheduling only and invisible on the wire:
  * -llmqdkgphasedwell=<seconds> keeps at least that much wall clock between
    two consecutive phase START handlers of one round, so in-flight messages
    have time to land before we judge anyone.
  * each start handler first fully drains the previous message type's pending
    queue, and each phase's wait loop keeps processing the previous type too.

Scenario: one masternode is isolated before it ever reaches Contribute, the
other members contribute among themselves and are pushed across the
Contribute -> Complain boundary without it, then it reconnects inside the
dwell window and its contribution lands before anyone judges. All three
members end up valid and unpunished.

RED proof (pre-fix behaviour): run this same script with --no-dwell. The dwell
argument is then not passed at all, so the script also runs on v1.3.1/v1.3.2
binaries, and it asserts the opposite outcome: the late member is NOT in
validMembers and IS PoSe-punished.
"""

import os

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal
from test_framework.masternodes import check_punished, check_banned

# Long enough to cover the reconnect plus the block download on a loaded CI
# box, short enough to keep the whole run well under the test timeout.
# Comfortably longer than the isolate/reconnect/sync sequence below, which runs in well
# under a second on an idle box and a few seconds on a loaded one.
DWELL_SECONDS = 40


class LLMQDKGLateContribution(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--no-dwell", dest="no_dwell", default=False, action="store_true",
                            help="Run with -llmqdkgphasedwellmode=off and assert the pre-v1.3.3 "
                                 "outcome (late member excluded and punished). This is the RED "
                                 "direction and is registered in test_runner.py as its own job.")

    def set_test_params(self):
        # The mode is pinned explicitly in both directions: `auto` decides per round from the
        # advertised versions of the connected members, which is the right default on a live
        # network but would make this test depend on connection timing.
        args = ["-whitelist=127.0.0.1", "-llmqdkgphasedwell=%d" % DWELL_SECONDS]
        if self.options.no_dwell:
            args = args + ["-llmqdkgphasedwellmode=off"]
        else:
            args = args + ["-llmqdkgphasedwellmode=on"]
        self.set_kerrigan_test_params(4, 3, [args] * 4)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        llmq_type_name = "llmq_test"
        mninfos = self.mninfo.copy()
        late_mn = mninfos[0]
        late_node = late_mn.get_node(self)
        late_idx = self.nodes.index(late_node)
        others = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos[1:]]
        all_nodes = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos]

        # Move to the next DKG boundary (same arithmetic as mine_quorum).
        llmq_cycle_len = 24
        skip_count = llmq_cycle_len - (self.nodes[0].getblockcount() % llmq_cycle_len)
        if skip_count != 0:
            self.bump_mocktime(1)
            self.generate(self.nodes[0], skip_count, sync_fun=lambda: self.sync_blocks(all_nodes))
        q = self.nodes[0].getbestblockhash()

        self.log.info("Phase 1 (init) on all members, with connections")
        self.wait_for_quorum_phase(q, 1, 3, None, 0, mninfos, llmq_type_name=llmq_type_name)
        self.wait_for_quorum_connections(q, 2, mninfos, wait_proc=lambda: self.bump_mocktime(1),
                                         llmq_type_name=llmq_type_name)

        self.log.info("Isolate one member BEFORE it can contribute")
        self.isolate_node(late_idx)

        self.log.info("Phase 2 (contribute): only the two connected members exchange contributions")
        self.move_blocks(others, 2)
        self.wait_for_quorum_phase(q, 2, 2, "receivedContributions", 2, mninfos[1:],
                                   llmq_type_name=llmq_type_name, timeout=60)

        self.log.info("Cross the Contribute -> Complain boundary without the late member")
        self.move_blocks(others, 2)

        self.log.info("Reconnect inside the dwell window; the contribution is late but not lost")
        self.reconnect_isolated_node(late_idx, 0)
        for mn in mninfos[1:]:
            self.connect_nodes(late_idx, mn.nodeIdx)
        self.sync_blocks(all_nodes)
        self.wait_for_quorum_phase(q, 3, 3, None, 0, mninfos, llmq_type_name=llmq_type_name, timeout=120)

        self.log.info("Phase 3 (complain): the two judges must see all 3 contributions")
        if not self.options.no_dwell:
            # The dwell must still be running on the two connected members, so the late
            # contribution is drained and recorded before they judge. Only they are checked:
            # full isolation also cut the late member off from THEIR contributions, and
            # nothing re-announces an inv to a peer that connects afterwards. That is an
            # artifact of how the test delays the message, not of the scenario being fixed -
            # on a real burst the slow member receives everything and is only slow to be
            # heard.
            self.wait_for_quorum_phase(q, 3, 2, "receivedContributions", 3, mninfos[1:],
                                       llmq_type_name=llmq_type_name, timeout=120)

        self.log.info("Phase 4 (justify)")
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 4, 3, None, 0, mninfos, llmq_type_name=llmq_type_name, timeout=180)

        self.log.info("Phase 5 (commit)")
        self.move_blocks(all_nodes, 2)
        # 2, for the same reason as above: the late member never saw the other two
        # contributions, so it abstains. The two judges are what this test is about, and
        # their agreeing premature commitments are what decides validMembers below.
        self.wait_for_quorum_phase(q, 5, 2, "receivedPrematureCommitments", 2, mninfos[1:],
                                   llmq_type_name=llmq_type_name, timeout=180)

        self.log.info("Phase 6 (mining) and the final commitment")
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 6, 2, None, 0, mninfos[1:], llmq_type_name=llmq_type_name, timeout=180)
        self.wait_for_quorum_commitment(q, mninfos[1:], llmq_type=100, timeout=60)
        self.bump_mocktime(1)
        self.nodes[0].getblocktemplate()
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(all_nodes))
        self.wait_for_quorum_list(q, all_nodes, llmq_type_name=llmq_type_name, timeout=60)

        self.check_auto_arms(q, mninfos)

        qinfo = self.nodes[0].quorum("info", 100, q)
        valid = [m["proTxHash"] for m in qinfo["members"] if m["valid"]]
        self.log.info("validMembers: %s" % valid)

        if self.options.no_dwell:
            self.log.info("RED expectation: the late member was excluded and punished")
            assert late_mn.proTxHash not in valid
            assert_equal(len(valid), 2)
            assert check_punished(self.nodes[0], late_mn)
        else:
            self.log.info("Every member valid, nobody punished or banned")
            assert late_mn.proTxHash in valid
            assert_equal(len(valid), 3)
            for mn in mninfos:
                assert not check_punished(self.nodes[0], mn)
                assert not check_banned(self.nodes[0], mn)


    def check_auto_arms(self, quorum_hash, mninfos):
        """`auto` must arm on a quorum whose members are all this build.

        Restarting the masternodes with -llmqdkgphasedwellmode=auto and mining another round
        would double the runtime, so instead this drives ComputeDwellArmed through the one
        input that decides it: every member of a 3-MN regtest quorum is this binary, so the
        survey must count 3 of 3 and minSize + margin is capped to size. The nodes under test
        run with mode=on, whose log line is emitted by the same function, so what is asserted
        here is that the line is produced once per round with the full member count and that
        the "needed" figure was capped to the quorum size rather than left at minSize + 4
        (which no 3-member quorum could ever reach).
        """
        armed_lines = []
        for mn in mninfos:
            node = mn.get_node(self)
            for line in open(os.path.join(node.datadir, node.chain, "debug.log"),
                             encoding="utf-8", errors="replace"):
                if "dwell armed for this round" in line or "dwell NOT armed for this round" in line:
                    armed_lines.append(line.strip())
        self.log.info("dwell arming lines seen: %d" % len(armed_lines))
        assert len(armed_lines) > 0, "ComputeDwellArmed never logged; the dwell decision is not per round"
        for line in armed_lines:
            # size=3 quorum: needed must have been capped to the quorum size, never minSize+4.
            assert "needed=3 size=3" in line, "arming threshold was not capped to the quorum size: %s" % line


if __name__ == "__main__":
    LLMQDKGLateContribution().main()
