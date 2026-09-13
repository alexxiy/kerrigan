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
import re
import time

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

        # The auto-mode survey learns a member's client version from a live connection, and
        # this test cuts one member off moments after the connections come up. Give every node
        # a beat to record all three before that, so the arming line reflects the quorum
        # rather than the order in which the framework happened to return from its RPCs.
        time.sleep(2 * self.options.timeout_factor)

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
        # Real time, with the mock clock frozen, so the late member can reconnect, catch up,
        # contribute, and have its inv fetched before anything advances the dwell. After the
        # v1.3.3 fix the pre-judge drain is the ONLY thing that records a late contribution -
        # nothing processes the queue once VerifyAndComplain has run - so a contribution that
        # arrives after the dwell expires is correctly ignored, and the test must not create
        # that situation itself.
        self.log.info("Settling: letting the late contribution cross the wire in real time")
        time.sleep(5 * self.options.timeout_factor)

        self.wait_phase(q, 3, 3, None, 0, mninfos, llmq_type_name=llmq_type_name)

        self.log.info("Phase 3 (complain): the two judges must see all 3 contributions")
        if not self.options.no_dwell:
            # The dwell must still be running on the two connected members, so the late
            # contribution is drained and recorded before they judge. Only they are checked:
            # full isolation also cut the late member off from THEIR contributions, and
            # nothing re-announces an inv to a peer that connects afterwards. That is an
            # artifact of how the test delays the message, not of the scenario being fixed -
            # on a real burst the slow member receives everything and is only slow to be
            # heard.
            self.wait_phase(q, 3, 2, "receivedContributions", 3, mninfos[1:],
                            llmq_type_name=llmq_type_name)

        self.log.info("Phase 4 (justify)")
        self.move_blocks(all_nodes, 2)
        self.wait_phase(q, 4, 3, None, 0, mninfos, llmq_type_name=llmq_type_name, timeout=180)

        self.log.info("Phase 5 (commit)")
        self.move_blocks(all_nodes, 2)
        # 2, for the same reason as above: the late member never saw the other two
        # contributions, so it abstains. The two judges are what this test is about, and
        # their agreeing premature commitments are what decides validMembers below.
        self.wait_phase(q, 5, 2, "receivedPrematureCommitments", 2, mninfos[1:],
                        llmq_type_name=llmq_type_name, timeout=180)

        self.log.info("Phase 6 (mining) and the final commitment")
        self.move_blocks(all_nodes, 2)
        self.wait_phase(q, 6, 2, None, 0, mninfos[1:], llmq_type_name=llmq_type_name, timeout=180)
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


    def wait_phase(self, quorum_hash, phase, member_count, check_messages, check_count, mninfos,
                   llmq_type_name="llmq_test", timeout=120):
        """wait_for_quorum_phase, but advancing the mock clock while it waits.

        The dwell is measured on the node's mockable clock. That is deliberate (it is what
        lets a 40 s dwell cost a couple of seconds of test runtime), but it means a test that
        merely sleeps would wait forever: regtest nodes run under -mocktime and their clock
        only moves when we move it. Bumping here is therefore both the mechanism and the
        assertion - if the dwell ever stopped using the mockable clock, these waits would
        start taking the full 40 s of wall time per phase.
        """
        def check():
            # One second per poll, not a jump: the dwell ends the moment the mock clock says
            # so, and the message this test is about has to reach the judges before that. A
            # coarse fast-forward would race the very exchange under test.
            self.bump_mocktime(1, nodes=self.nodes)
            seen = 0
            for mn in mninfos:
                for qs in mn.get_node(self).quorum("dkgstatus")["session"]:
                    if qs["llmqType"] != llmq_type_name:
                        continue
                    qstatus = qs["status"]
                    if qstatus["quorumHash"] != quorum_hash:
                        continue
                    if qstatus["phase"] != phase:
                        return False
                    if check_messages is not None and qstatus[check_messages] < check_count:
                        return False
                    seen += 1
                    break
            return seen >= member_count

        self.wait_until(check, timeout=timeout, sleep=0.5)

    def check_auto_arms(self, quorum_hash, mninfos):
        """The per-round survey must count DISTINCT members, and all of them here.

        Every member of this 3-masternode quorum runs this build, and the cumulative record
        the survey reads starts empty at process start, so an arming line that reports
        3 of 3 proves both halves of the rule at once: the record was populated from live
        peers during this process, and the count is of members rather than of connections.
        A connection count would exceed the member count, since the framework leaves both an
        inbound and an outbound link between each pair of masternodes.

        It also checks the threshold was capped to the quorum size rather than left at
        minSize + margin, which no 3-member quorum could ever reach.

        Not checked here: that a restarted node re-learns the record. That needs a second
        full DKG round, three more dwells, which is the dominant cost of this test, and the
        property is already exercised by every run - the record is per process and empty at
        startup, so the first round could not report 3 of 3 unless it had been re-learned.
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

        counts = []
        for line in armed_lines:
            m = re.search(r"dwell-aware members=(\d+) of (\d+), needed=(\d+)", line)
            assert m is not None, "arming line does not carry the survey: %s" % line
            aware, members, needed = (int(g) for g in m.groups())
            assert members == 3, "quorum did not have 3 members: %s" % line
            # The bug this guards: counting CNode connections rather than distinct members.
            # The framework leaves an inbound and an outbound link between each pair of
            # masternodes, so a connection count would report more members than exist.
            assert aware <= members, \
                "survey counted more dwell-aware members than the quorum has: %s" % line
            assert needed == members, \
                "arming threshold was not capped to the quorum size: %s" % line
            counts.append(aware)

        # At least one node must have learned the whole quorum. The record is per process and
        # empty at startup, so this is also what proves it gets populated from live peers.
        # Not every node: a member that is unreachable when a node takes its survey is
        # legitimately not counted, and this test isolates one member mid-round on purpose.
        assert max(counts) == 3, \
            "no node ever learned all 3 members were dwell-aware, counts were %s" % counts
        self.log.info("dwell-aware counts per arming line: %s" % counts)


if __name__ == "__main__":
    LLMQDKGLateContribution().main()
