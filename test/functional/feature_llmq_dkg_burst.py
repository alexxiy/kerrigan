#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_llmq_dkg_burst.py

v1.3.2 DKG burst tolerance.

On Kerrigan, block timing is erratic: a long gap followed by a burst. A burst
connects several blocks in ONE ActivateBestChain batch, which fires a single
UpdatedBlockTip at the final tip. The DKG phase is height/dkgPhaseBlocks, so a
member whose tip jumps across a phase boundary sees its phase scalar jump past
the one it is waiting for. Pre-v1.3.2 that threw AbortPhaseException and the
member sat out the whole 24-block round (no premature commitment sent) -- and
when enough members did so at different points, no commitment formed and the
dropped members were PoSe-penalized.

This test reproduces the batch with the framework's isolate/reconnect helpers:
one masternode is isolated after contributions were exchanged, the rest of the
network mines two phases (Complain + Justify) without it, then it reconnects
and downloads the 4 blocks in a single batch -> its phase jumps 2 -> 4.

With the v1.3.2 fix the member catches up (runs the skipped phase handlers in
order) and still SENDS its premature commitment, so every member receives 3
premature commitments (the full quorum) and its dkgstatus shows the session
was not aborted. Pre-fix this same script sees only 2 premature commitments
and the isolated member's session flagged aborted (the RED proof, which needs
an unpatched binary and is therefore documented rather than run here).
"""

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal
from test_framework.masternodes import check_punished, check_banned


class LLMQDKGBurst(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_kerrigan_test_params(4, 3, [["-whitelist=127.0.0.1"]] * 4)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("Baseline: one clean quorum forms with every member valid")
        self.mine_quorum()

        self.log.info("Burst round: isolate one member across a phase boundary")
        llmq_type_name = "llmq_test"
        mninfos = self.mninfo.copy()
        burst_mn = mninfos[0]
        burst_node = burst_mn.get_node(self)
        burst_idx = self.nodes.index(burst_node)
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
        self.wait_for_quorum_connections(q, 2, mninfos, wait_proc=lambda: self.bump_mocktime(1), llmq_type_name=llmq_type_name)
        self.move_blocks(all_nodes, 2)

        self.log.info("Phase 2 (contribute): all 3 contributions exchanged BEFORE the burst")
        self.wait_for_quorum_phase(q, 2, 3, "receivedContributions", 3, mninfos, llmq_type_name=llmq_type_name)

        self.log.info("Isolate the burst member, advance the rest through Complain and Justify")
        self.isolate_node(burst_idx)
        self.move_blocks(others, 2)  # -> phase 3 (complain) for the others
        self.wait_for_quorum_phase(q, 3, 2, "receivedComplaints", 0, mninfos[1:], llmq_type_name=llmq_type_name)
        self.move_blocks(others, 2)  # -> phase 4 (justify) for the others
        self.wait_for_quorum_phase(q, 4, 2, "receivedJustifications", 0, mninfos[1:], llmq_type_name=llmq_type_name)

        self.log.info("Reconnect: the member downloads 4 blocks in ONE batch -> phase jumps 2 -> 4")
        self.reconnect_isolated_node(burst_idx, 0)
        self.sync_blocks(all_nodes)
        # The burst member must arrive at phase 4 WITHOUT aborting its session.
        self.wait_for_quorum_phase(q, 4, 3, None, 0, mninfos, llmq_type_name=llmq_type_name)

        self.log.info("Phase 5 (commit): the burst member still sends its premature commitment")
        self.move_blocks(all_nodes, 2)
        # 3 premature commitments = the full quorum, including the burst member.
        # Pre-fix this times out at 2 (the burst member aborted in phase 2->4).
        self.wait_for_quorum_phase(q, 5, 3, "receivedPrematureCommitments", 3, mninfos, llmq_type_name=llmq_type_name)

        # Belt-and-suspenders on top of the phase-5 wait above (which already
        # required 3/3 premature commitments INCLUDING the burst member -- the
        # load-bearing proof that the fix let it catch up instead of aborting):
        # if the burst member's session for this round is still live in
        # dkgstatus, it must show the round was NOT aborted and its premature
        # commitment WAS sent. dkgstatus.session is a list of
        # {llmqType, quorumIndex, status:{quorumHash, aborted, sentPrematureCommitment,...}}
        # and rotates out once the round completes, so absence is acceptable.
        status = burst_node.quorum("dkgstatus")
        sessions = status.get("session") or []
        self.log.info("burst member dkgstatus session: %s" % sessions)
        for sess in sessions:
            inner = sess.get("status", sess) if isinstance(sess, dict) else {}
            if inner.get("quorumHash") == q:
                assert_equal(inner.get("aborted", False), False)
                assert_equal(inner.get("sentPrematureCommitment", False), True)
                self.log.info("burst member reached commit without aborting the round")

        self.log.info("Phase 6 (mining) and the final commitment")
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 6, 3, None, 0, mninfos, llmq_type_name=llmq_type_name)
        self.wait_for_quorum_commitment(q, mninfos, llmq_type=100)
        self.bump_mocktime(1)
        self.nodes[0].getblocktemplate()
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(all_nodes))
        self.wait_for_quorum_list(q, all_nodes, llmq_type_name=llmq_type_name)

        self.log.info("Every member valid, nobody punished or banned")
        qinfo = self.nodes[0].quorum("info", 100, q)
        valid = [m for m in qinfo["members"] if m["valid"]]
        assert_equal(len(valid), 3)
        for mn in mninfos:
            assert not check_punished(self.nodes[0], mn)
            assert not check_banned(self.nodes[0], mn)


if __name__ == "__main__":
    LLMQDKGBurst().main()
