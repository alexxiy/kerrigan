#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_llmq_dkg_late_contribution.py

Regression test for the contract around a contribution that misses its phase.

A DKG phase is dkgPhaseBlocks blocks wide. On Kerrigan a multi-algo block burst
can connect a whole phase in seconds, so the Complain start handler
(VerifyAndComplain) can run while another member's contribution is still in
flight. VerifyAndComplain marks every member with no contribution as bad, and
once dkgBadVotesThreshold members agree, the victim is dropped from validMembers
of the mined commitment and takes a PoSe penalty.

v1.3.3 keeps two local-scheduling changes in this area, neither visible on the
wire and neither changing block validity:
  * each phase START handler fully drains the previous message type's pending
    queue before it judges anyone, so a message already sitting in our own
    queue is never counted as absent, and
  * a node keeps answering requests for its contribution one phase longer (and
    for its premature commitment into finalisation) so a member that caught up
    after a burst can still be fetched.

What this test does NOT claim: that either change rescues the member below. The
phase-dwell that did rescue it was removed before release (it converted null
rounds into partial commitments and punished the members still in flight, which
is worse than the baseline). On regtest the two surviving changes cannot be
distinguished from v1.3.2 by outcome: the drain only matters when a message is
already queued at the instant the start handler runs, and the framework has no
way to inject a DKG message at that instant or to request one out of phase. So
this is a REGRESSION test, and it pins the v1.3.2 contract in both directions:

  1. A clean round still forms with every member valid and nobody punished.
     This is what would break if the drain or the narrowed serving windows
     regressed the ordinary path.
  2. A member whose contribution misses the Complain start is still excluded
     from validMembers and still PoSe-punished, and the remaining members still
     mine a commitment. This is the v1.3.2 behaviour, unchanged, and it is
     asserted so that any future attempt to soften the judgement has to change
     this file deliberately rather than silently.
"""

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal
from test_framework.masternodes import check_punished, check_banned


class LLMQDKGLateContribution(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_kerrigan_test_params(4, 3, [["-whitelist=127.0.0.1"]] * 4)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        llmq_type_name = "llmq_test"
        mninfos = self.mninfo.copy()

        self.log.info("Baseline: a clean round forms with every member valid")
        self.mine_quorum()
        for mn in mninfos:
            assert not check_punished(self.nodes[0], mn)
            assert not check_banned(self.nodes[0], mn)

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

        self.log.info("Reconnect: too late to be counted, but the round must not stall")
        self.reconnect_isolated_node(late_idx, 0)
        for mn in mninfos[1:]:
            self.connect_nodes(late_idx, mn.nodeIdx)
        self.sync_blocks(all_nodes)
        self.wait_for_quorum_phase(q, 3, 3, None, 0, mninfos, llmq_type_name=llmq_type_name, timeout=120)

        self.log.info("Phase 4 (justify)")
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 4, 3, None, 0, mninfos, llmq_type_name=llmq_type_name, timeout=120)

        self.log.info("Phase 5 (commit)")
        self.move_blocks(all_nodes, 2)
        # 2, not 3: the late member never saw the other two contributions either, so it
        # abstains. The two judges are what decides validMembers below.
        self.wait_for_quorum_phase(q, 5, 2, "receivedPrematureCommitments", 2, mninfos[1:],
                                   llmq_type_name=llmq_type_name, timeout=120)

        self.log.info("Phase 6 (mining) and the final commitment")
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 6, 2, None, 0, mninfos[1:], llmq_type_name=llmq_type_name, timeout=120)
        self.wait_for_quorum_commitment(q, mninfos[1:], llmq_type=100, timeout=60)
        self.bump_mocktime(1)
        self.nodes[0].getblocktemplate()
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(all_nodes))
        self.wait_for_quorum_list(q, all_nodes, llmq_type_name=llmq_type_name, timeout=60)

        qinfo = self.nodes[0].quorum("info", 100, q)
        valid = [m["proTxHash"] for m in qinfo["members"] if m["valid"]]
        self.log.info("validMembers: %s" % valid)

        self.log.info("v1.3.2 contract: the late member is excluded and punished")
        assert late_mn.proTxHash not in valid
        assert_equal(len(valid), 2)
        assert check_punished(self.nodes[0], late_mn)
        for mn in mninfos[1:]:
            assert not check_punished(self.nodes[0], mn)
            assert not check_banned(self.nodes[0], mn)


if __name__ == "__main__":
    LLMQDKGLateContribution().main()
