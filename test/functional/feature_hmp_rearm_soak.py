#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_hmp_rearm_soak.py  --  v1.3.2 "testnet drill" at functional scale.

Runs the DKG + HMP re-arm machinery over MULTIPLE quorum cycles with the re-arm
fork ACTIVE (coinbases are v6, VRF proofs/commitment-sigs/identity-sig committed
on chain), and injects a block-burst across a DKG phase boundary in one cycle.
Asserts across the whole soak: every quorum forms, and NO masternode is ever
PoSe-punished or banned -- i.e. the phase-skip liveness fix + penalty
recalibration + v6 economics all hold together over repeated cycles.

This is the feasible-scale stand-in for the live 40-node llmq_60_60 testnet soak
(minSize 40 cannot form locally); it exercises the identical DKG / re-arm code
paths with the small llmq_test quorum.
"""

from test_framework.test_framework import KerriganTestFramework
from test_framework.util import assert_equal, assert_greater_than
from test_framework.masternodes import check_punished, check_banned

REARM = 250


class HMPRearmSoak(KerriganTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_kerrigan_test_params(
            4, 3, [["-testactivationheight=hmp_rearm@%d" % REARM]] * 4)

    def assert_clean(self):
        for mn in self.mninfo:
            assert not check_punished(self.nodes[0], mn), "a masternode was PoSe-punished"
            assert not check_banned(self.nodes[0], mn), "a masternode was PoSe-banned"

    def cbtx_version_tip(self):
        n = self.nodes[0]
        cb = n.getblock(n.getbestblockhash(), 2)["tx"][0]
        return (cb.get("cbTx") or {}).get("version")

    def burst_cycle(self):
        """One DKG cycle where a member is isolated across a phase boundary and
        reconnected so it downloads the blocks in one batch (phase jumps 2->4)."""
        name = "llmq_test"
        mninfos = self.mninfo.copy()
        burst_idx = self.nodes.index(mninfos[0].get_node(self))
        others = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos[1:]]
        all_nodes = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos]

        skip = 24 - (self.nodes[0].getblockcount() % 24)
        if skip:
            self.bump_mocktime(1)
            self.generate(self.nodes[0], skip, sync_fun=lambda: self.sync_blocks(all_nodes))
        q = self.nodes[0].getbestblockhash()
        self.wait_for_quorum_phase(q, 1, 3, None, 0, mninfos, llmq_type_name=name)
        self.wait_for_quorum_connections(q, 2, mninfos, wait_proc=lambda: self.bump_mocktime(1), llmq_type_name=name)
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 2, 3, "receivedContributions", 3, mninfos, llmq_type_name=name)

        self.isolate_node(burst_idx)
        self.move_blocks(others, 2)
        self.wait_for_quorum_phase(q, 3, 2, "receivedComplaints", 0, mninfos[1:], llmq_type_name=name)
        self.move_blocks(others, 2)
        self.wait_for_quorum_phase(q, 4, 2, "receivedJustifications", 0, mninfos[1:], llmq_type_name=name)
        self.reconnect_isolated_node(burst_idx, 0)
        self.sync_blocks(all_nodes)
        self.wait_for_quorum_phase(q, 4, 3, None, 0, mninfos, llmq_type_name=name)

        self.move_blocks(all_nodes, 2)
        # 3/3 premature commitments incl. the burst member: the fix caught it up.
        self.wait_for_quorum_phase(q, 5, 3, "receivedPrematureCommitments", 3, mninfos, llmq_type_name=name)
        self.move_blocks(all_nodes, 2)
        self.wait_for_quorum_phase(q, 6, 3, None, 0, mninfos, llmq_type_name=name)
        self.wait_for_quorum_commitment(q, mninfos, llmq_type=100)
        self.bump_mocktime(1)
        self.nodes[0].getblocktemplate()
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(all_nodes))
        self.wait_for_quorum_list(q, all_nodes, llmq_type_name=name)
        return q

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        # Setup already mined well past REARM=250, so the re-arm regime is active.
        assert_greater_than(self.nodes[0].getblockcount(), REARM)
        assert_equal(self.nodes[0].gethmpinfo()["rearm_active"], True)
        assert_equal(self.cbtx_version_tip(), 6)
        self.log.info("Re-arm regime ACTIVE (v6 coinbases) before the soak")

        formed = []
        self.log.info("Soak cycle 1/4: clean quorum")
        formed.append(self.mine_quorum()); self.assert_clean()
        self.log.info("Soak cycle 2/4: BURST quorum")
        formed.append(self.burst_cycle()); self.assert_clean()
        self.log.info("Soak cycle 3/4: clean quorum")
        formed.append(self.mine_quorum()); self.assert_clean()
        self.log.info("Soak cycle 4/4: BURST quorum")
        formed.append(self.burst_cycle()); self.assert_clean()

        assert_equal(len(set(formed)), 4)
        assert_equal(self.cbtx_version_tip(), 6)
        # Every quorum still valid, no bans accrued over the whole soak.
        for q in formed:
            info = self.nodes[0].quorum("info", 100, q)
            assert_equal(len([m for m in info["members"] if m["valid"]]), 3)
        self.assert_clean()
        self.log.info("HMP re-arm soak: 4 quorums formed (2 under burst), zero PoSe events")


if __name__ == "__main__":
    HMPRearmSoak().main()
