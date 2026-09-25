#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Rehearse turning HMP on from OFF, on a chain already past the re-arm fork.

This is mainnet's state today: v1.3.2, the h148000 re-arm active, and
SPORK_25_HMP_ENABLED off, so no node signs seals, no miner ever becomes an
Elder and the penalty never arms. SPORK_25 gates production only (commitment
broadcast, seal signing, HMP P2P handling); validation never reads it. Regtest
defaults the spork ON, so no other test has ever exercised the off->on
transition that mainnet would go through.

Phases:
  A. Spork OFF, both nodes restarted so their spork cache reads OFF at init.
     Paced mining must produce no seals and no Elders.
  B. Spork flipped ON with NO restart. This is measured, not asserted: whether
     anything starts without a restart decides the operational plan, i.e.
     whether every pool daemon has to be restarted after the mainnet flip.
  C. Both nodes restarted with the spork ON. Both identities must reach Elder
     on x11, 2-signer seals must appear, and the penalty must stay unarmed (it
     needs 4 organic Elders; there are 2).

Mining a block is itself a commitment: CHMPCommitmentRegistry::BlockConnected
registers the v6 coinbase's miner identity during validation, whatever the spork
says. So every mining pool is already committed on mainnet today, and only a
non-mining node (like a seed) depends on the startup self-commitment that the
spork gates.
  D. Consensus safety. node1 rebuilds its chainstate from disk and must reach
     the identical chain_seal_work at the identical tip, over a span that is
     required to contain sealed blocks (with no seals every weight is the
     neutral 10000 and agreement would prove nothing). Then the spork is
     flipped back OFF: the chain must keep advancing, the nodes must still
     agree, and signing must stop.

Only two signer nodes: every regtest node shares 127.0.0.1 and the SEALSHARE
handler rate-limits per IP, so a third signer would drop the others' shares.
Seal assembly is wall-clock, so blocks are paced with real sleeps.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"
SPORK_ON = 0
SPORK_OFF = 4070908800
REARM = 30
SIGNING_WINDOW_MS = 300
BLOCK_PACE = 1.2
PRIVILEGE_WINDOW = 16
NEUTRAL_WEIGHT = 10000
AGE_OUT_BLOCKS = 1200


def base_args():
    return [
        "-dip3params=2:2",
        "-testactivationheight=v20@2",
        "-testactivationheight=hmp_rearm@%d" % REARM,
        "-testactivationheight=hmp_deterministic_seal@1",
        "-debug=hmp",
        "-hmpsigningwindowms=%d" % SIGNING_WINDOW_MS,
        "-hmpprivilegewindow=%d" % PRIVILEGE_WINDOW,
        "-fallbackfee=0.00001",
    ]


class HMPSpork25FlipTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.disable_mocktime = True
        self.node_args = [base_args() + ["-sporkkey=" + SPORK_KEY], base_args()]
        self.extra_args = self.node_args

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # --- helpers ---------------------------------------------------------

    def set_spork(self, value):
        self.nodes[0].sporkupdate("SPORK_25_HMP_ENABLED", value)
        for node in self.nodes:
            self.wait_until(lambda n=node: n.spork("show")["SPORK_25_HMP_ENABLED"] == value)

    def restart_both(self):
        for i in range(2):
            self.restart_node(i, extra_args=self.node_args[i])
        self.connect_nodes(0, 1)
        # A restart can race the old process for the listen port; do not let a
        # missing peer masquerade as a chain problem later.
        self.wait_until(lambda: all(n.getconnectioncount() > 0 for n in self.nodes), timeout=60)
        self.sync_blocks()

    def mine_paced(self, count):
        """Alternate the miner, sync, then sleep past the signing window so
        shares are exchanged and the seal is assembled."""
        for i in range(count):
            node = self.nodes[i % 2]
            self.generatetoaddress(node, 1, self.addrs[i % 2], sync_fun=self.no_op)
            self.sync_blocks()
            time.sleep(BLOCK_PACE)

    def sealed_heights(self, lo, hi):
        node = self.nodes[0]
        return [h for h in range(lo, hi + 1)
                if node.getsealstatus(node.getblockhash(h))["has_seal"]]

    def committed(self, node):
        return bool(node.gethmpdiagnostics().get("commitment", {}).get("committed"))

    def elder_count(self, view_node):
        # gethmpidentity and gethmpprivilegedset serialize the same BLS key with
        # different scheme flag bits in the leading byte, so compare the rest.
        ids = {self.ids[0][2:], self.ids[1][2:]}
        entries = view_node.gethmpprivilegedset("x11")["x11"]
        return sum(1 for e in entries if e["pubkey"][2:] in ids and e["tier"] == "elder")

    def x11_info(self, node):
        return next(a for a in node.gethmpinfo()["algos"] if a["name"] == "x11")

    # --- test --------------------------------------------------------------

    def run_test(self):
        self.connect_nodes(0, 1)
        self.addrs = [n.getnewaddress() for n in self.nodes]
        self.ids = [n.gethmpidentity()["identity"] for n in self.nodes]
        assert self.ids[0] != self.ids[1]

        self.log.info("Setup: SPORK_25 OFF, restart so init reads OFF")
        self.set_spork(SPORK_OFF)
        self.restart_both()
        for node in self.nodes:
            assert_equal(node.spork("show")["SPORK_25_HMP_ENABLED"], SPORK_OFF)

        self.log.info("Setup: mine past the re-arm height (%d) with the spork off", REARM)
        start = self.nodes[0].getblockcount() + 1
        # Sync after each batch so both miners build on the shared tip.
        self.generatetoaddress(self.nodes[0], REARM + 5, self.addrs[0])
        self.generatetoaddress(self.nodes[1], 5, self.addrs[1])
        assert_equal(self.nodes[0].gethmpinfo()["rearm_active"], True)
        # Explicit commitments ride in the coinbase and ARE gated by the spork.
        embedded = sum(1 for h in range(start, self.nodes[0].getblockcount() + 1)
                       if (self.nodes[0].getblock(self.nodes[0].getblockhash(h), 2)
                           ["tx"][0].get("cbTx") or {}).get("vCommitments"))
        assert_equal(embedded, 0)

        # ---- A ----
        self.log.info("A: paced mining with the spork OFF")
        a_lo = self.nodes[0].getblockcount() + 1
        self.mine_paced(40)
        a_hi = self.nodes[0].getblockcount()
        assert_equal(self.sealed_heights(a_lo, a_hi), [])
        assert_equal(self.elder_count(self.nodes[1]), 0)
        # Mining a block IS a commitment: CHMPCommitmentRegistry::BlockConnected
        # registers the v6 coinbase's miner identity, in validation, regardless of
        # the spork. So a miner is committed even with HMP off -- exactly the state
        # every mainnet pool is in today. (A NON-mining node, like a seed, is not.)
        for node in self.nodes:
            assert self.committed(node), "a miner should be implicitly committed by mining"
        self.log.info("A: %d blocks with the spork OFF: no seals, no Elders; miners implicitly "
                      "committed, explicit commitments embedded: %d", a_hi - a_lo + 1, embedded)

        # ---- B ----
        self.log.info("B: flip SPORK_25 ON with NO restart, and measure")
        self.set_spork(SPORK_ON)
        b_lo = self.nodes[0].getblockcount() + 1
        self.mine_paced(40)
        b_hi = self.nodes[0].getblockcount()
        b_sealed = self.sealed_heights(b_lo, b_hi)
        b_committed = [self.committed(n) for n in self.nodes]
        b_elders = self.elder_count(self.nodes[1])
        self.log.info("B RESULT (no restart): committed=%s elders=%d sealed_blocks=%d of %d",
                      b_committed, b_elders, len(b_sealed), b_hi - b_lo + 1)
        # Miners are committed either way (mining is an implicit commitment), so the
        # honest signal is whether seals and Elders appear without a restart.
        self.b_restart_needed = not b_sealed and b_elders == 0

        # ---- C ----
        self.log.info("C: restart both nodes with the spork ON")
        self.restart_both()
        c_lo = self.nodes[0].getblockcount() + 1
        for _ in range(12):
            self.mine_paced(10)
            if (all(self.committed(n) for n in self.nodes) and
                    self.elder_count(self.nodes[1]) == 2 and
                    self.sealed_heights(c_lo, self.nodes[0].getblockcount() - 2)):
                break
        c_hi = self.nodes[0].getblockcount()
        for node in self.nodes:
            assert self.committed(node), "identity never committed after restart"
        assert_equal(self.elder_count(self.nodes[1]), 2)
        c_sealed = self.sealed_heights(c_lo, c_hi)
        assert c_sealed, "no sealed block after both identities reached Elder"
        assert_equal(self.x11_info(self.nodes[0])["penalty_armed"], False)
        self.log.info("C: committed, 2 Elders on x11, %d sealed blocks in %d, penalty unarmed",
                      len(c_sealed), c_hi - c_lo + 1)

        # ---- D1 ----
        self.log.info("D1: node1 rebuilds its chainstate from disk; seal work must match")
        self.mine_paced(6)
        tip = self.nodes[0].getbestblockhash()
        top = self.nodes[0].getblockcount()
        span = range(c_lo, top + 1)
        before = {h: self.nodes[0].getsealstatus(self.nodes[0].getblockhash(h)) for h in span}
        sealed_in_span = [h for h in span if before[h]["seal_weight"] > NEUTRAL_WEIGHT]
        assert sealed_in_span, "no sealed blocks in the comparison span; agreement would be vacuous"
        # -reindex-chainstate refuses to run alongside -txindex, which the
        # framework enables by default.
        self.restart_node(1, extra_args=self.node_args[1] + ["-reindex-chainstate", "-txindex=0"])
        self.wait_until(lambda: self.nodes[1].getbestblockhash() == tip, timeout=180)
        for h in span:
            got = self.nodes[1].getsealstatus(self.nodes[1].getblockhash(h))
            assert_equal((got["seal_weight"], got["chain_seal_work"]),
                         (before[h]["seal_weight"], before[h]["chain_seal_work"]))
        self.connect_nodes(0, 1)
        self.log.info("D1: rebuilt node matches seal_weight and chain_seal_work on all %d blocks "
                      "(%d of them sealed)", len(span), len(sealed_in_span))

        # ---- D2 ----
        self.log.info("D2: flip SPORK_25 back OFF; chain must continue and nodes must agree")
        self.set_spork(SPORK_OFF)
        off_at = self.nodes[0].getblockcount()
        self.mine_paced(20)
        assert_greater_than(self.nodes[0].getblockcount(), off_at + 15)
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[1].getbestblockhash())
        tip = self.nodes[0].getbestblockhash()
        assert_equal(self.nodes[0].getsealstatus(tip)["chain_seal_work"],
                     self.nodes[1].getsealstatus(tip)["chain_seal_work"])
        # Seals trail their block by two heights; skip the first few after the flip.
        late = self.sealed_heights(off_at + 4, self.nodes[0].getblockcount())
        assert_equal(late, [])
        self.log.info("D2: rollback clean: chain advanced, nodes agree, no new seals")

        self.log.info("REHEARSAL SUMMARY: restart required after flip = %s", self.b_restart_needed)


if __name__ == "__main__":
    HMPSpork25FlipTest().main()
