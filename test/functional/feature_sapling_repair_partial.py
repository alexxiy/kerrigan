#!/usr/bin/env python3
"""The stale-spend repair must release notes one by one, never a whole transaction.

A dead transaction T spent notes A and B. A competing transaction spending only
A was mined, so A's nullifier is on chain and B's is not. B is stranded and must
come back. A is spent and must stay spent.

Before the fix the repair checked each note's nullifier, but then released every
note T had spent. B qualified, so A was released with it, and the wallet showed
funds that were already gone on chain.

The competing spend comes from a second node holding the same spending key. It
is built and mined while the nodes are disconnected, so the owner never sees it
until the block arrives, and it spends A alone because B is too young for its
minconf.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100
NOTE_A = Decimal("5.0")
NOTE_B = Decimal("2.0")


class SaplingRepairPartialTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-debug=sapling"], ["-debug=sapling"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        params_dir = os.path.expanduser("~/.kerrigan-params")
        for name in ("sapling-spend.params", "sapling-output.params"):
            if not os.path.exists(os.path.join(params_dir, name)):
                raise SkipTest("Sapling parameters not found. Run contrib/fetch-params.sh first.")

    def run_test(self):
        owner, rival = self.nodes
        t_addr = owner.getnewaddress()
        self.generatetoaddress(owner, MATURITY + 10, t_addr)

        z_addr = owner.z_getnewaddress()
        rival.z_importkey(owner.z_exportkey(z_addr), "no")

        self.log.info("Shield note A, age it, then shield note B")
        owner.z_sendmany(t_addr, [{"address": z_addr, "amount": float(NOTE_A)}])
        self.generatetoaddress(owner, 11, t_addr)
        owner.z_sendmany(t_addr, [{"address": z_addr, "amount": float(NOTE_B)}])
        self.generatetoaddress(owner, 1, t_addr)
        for node in self.nodes:
            node.syncwithvalidationinterfacequeue()
            assert_equal(Decimal(node.z_getbalance(z_addr)), NOTE_A + NOTE_B)

        self.disconnect_nodes(0, 1)

        self.log.info("The rival spends A alone and keeps it to itself")
        # minconf 5 leaves only A eligible (B has 1 confirmation). Sending all of
        # A with an explicit fee leaves no change note for the owner to pick up.
        fee = Decimal("0.0003")
        rival_t = rival.getnewaddress()
        rival_txid = rival.z_sendmany(z_addr, [{"address": rival_t, "amount": float(NOTE_A - fee)}], 5, float(fee))

        self.log.info("The owner spends A and B together, unmined")
        dead_txid = owner.z_sendmany(z_addr, [{"address": t_addr, "amount": 6.0}])
        owner.syncwithvalidationinterfacequeue()
        assert dead_txid in owner.getrawmempool()
        assert_equal(Decimal(owner.z_getbalance(z_addr)), Decimal("0"))

        self.log.info("The rival mines its spend of A; the owner's spend conflicts out")
        rival_block = self.generatetoaddress(rival, 1, rival_t, sync_fun=self.no_op)[0]
        assert rival_txid in rival.getblock(rival_block)["tx"]
        # The witness rebuild that follows the repair replays the rival's block
        # and marks A spent again, which hides a wrong release from the balance.
        # The repair's own count is the only place the bug shows reliably.
        with owner.assert_debug_log(expected_msgs=["Sapling: released 1 note(s)"],
                                    unexpected_msgs=["Sapling: released 2 note(s)"], timeout=30):
            self.connect_nodes(0, 1)
            self.sync_blocks()
            owner.syncwithvalidationinterfacequeue()
            assert dead_txid not in owner.getrawmempool(), "the conflicting spend should be gone"
            assert_equal(owner.getbestblockhash(), rival_block)

            # One more tip so the repair has certainly run after the eviction.
            self.generatetoaddress(owner, 1, t_addr)
            owner.syncwithvalidationinterfacequeue()

        unspent = owner.z_listunspent()
        values = sorted(Decimal(str(n["amount"])) for n in unspent)
        self.log.info("owner notes after repair: %s", values)
        assert_equal(values, [NOTE_B])
        assert_equal(Decimal(owner.z_getbalance(z_addr)), NOTE_B)

        self.log.info("B is genuinely spendable")
        owner.z_sendmany(z_addr, [{"address": t_addr, "amount": 1.0}])
        self.generatetoaddress(owner, 1, t_addr)
        owner.syncwithvalidationinterfacequeue()
        assert Decimal(owner.z_getbalance(z_addr)) < NOTE_B


if __name__ == "__main__":
    SaplingRepairPartialTest().main()
