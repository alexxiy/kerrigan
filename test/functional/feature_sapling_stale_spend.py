#!/usr/bin/env python3
"""Release Sapling notes whose spending transaction never confirms.

ScanSaplingSpends() marks a note spent as soon as the spending tx is seen,
including from the mempool. If that tx then leaves the mempool without being
mined (conflicted by a reorg, evicted, replaced, or simply dropped when the
node restarts without a persisted mempool), nothing used to clear the flag:
the note stayed spent forever, so the balance vanished and the funds could
never be spent again. That is the shape of the reported "notes unspendable
after a z->z self-send + restart" bug.

This test reproduces the drop-from-mempool case and asserts the balance comes
back. It would fail before CWallet::RepairStaleSaplingSpends existed.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100


class SaplingStaleSpendTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        params_dir = os.path.expanduser("~/.kerrigan-params")
        for name in ("sapling-spend.params", "sapling-output.params"):
            if not os.path.exists(os.path.join(params_dir, name)):
                raise SkipTest("Sapling parameters not found. Run contrib/fetch-params.sh first.")

    def run_test(self):
        node = self.nodes[0]
        t_addr = node.getnewaddress()
        self.generatetoaddress(node, MATURITY + 10, t_addr)

        z_addr = node.z_getnewaddress()
        shield_amount = Decimal("5.0")

        self.log.info("Shield %s to %s", shield_amount, z_addr)
        node.z_sendmany(t_addr, [{"address": z_addr, "amount": float(shield_amount)}])
        self.generatetoaddress(node, 1, t_addr)
        assert_equal(Decimal(node.z_getbalance(z_addr)), shield_amount)
        assert_equal(len(node.z_listunspent(1, 9999999, [z_addr])), 1)

        self.log.info("Unshield without mining it: the note is marked spent")
        spend_txid = node.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        node.syncwithvalidationinterfacequeue()
        assert spend_txid in node.getrawmempool()
        assert_equal(Decimal(node.z_getbalance(z_addr)), Decimal("0"))
        assert_equal(len(node.z_listunspent(0, 9999999, [z_addr])), 0)

        self.log.info("Drop the tx from the mempool: it will never confirm")
        # -walletbroadcast=0 too, or the wallet rebroadcasts its own
        # unconfirmed tx on startup and it lands back in the mempool.
        self.restart_node(0, extra_args=["-persistmempool=0", "-walletbroadcast=0"])
        node = self.nodes[0]
        assert spend_txid not in node.getrawmempool()
        assert_equal(node.gettransaction(spend_txid)["confirmations"], 0)

        self.log.info("A new tip triggers the repair; the note must come back")
        self.generatetoaddress(node, 1, t_addr)
        node.syncwithvalidationinterfacequeue()

        restored = Decimal(node.z_getbalance(z_addr))
        assert_equal(restored, shield_amount)
        unspent = node.z_listunspent(1, 9999999, [z_addr])
        assert_equal(len(unspent), 1)
        assert_equal(Decimal(str(unspent[0]["amount"])), shield_amount)
        self.log.info("Balance restored to %s and the note is spendable again", restored)

        self.log.info("And it really is spendable: unshield again, mined this time")
        node.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        self.generatetoaddress(node, 1, t_addr)
        assert_equal(Decimal(node.z_getbalance(z_addr)) < shield_amount, True)


if __name__ == "__main__":
    SaplingStaleSpendTest().main()
