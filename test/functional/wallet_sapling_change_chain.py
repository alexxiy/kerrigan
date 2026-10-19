#!/usr/bin/env python3
"""Change of change must stay attributed to its address however long the chain.

Change from a shielded send lands on the wallet's internal address. It belongs
to the external address whose notes paid for the send, and spending that change
produces more change, so ownership is found by walking back through the
generations. The walk used to stop at depth 32: from the 33rd generation on the
change belonged to no external address, z_getbalance dropped it and z_sendmany
could not select it.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100
GENERATIONS = 40
SHIELD = Decimal("10.0")
SEND = Decimal("0.01")


class SaplingChangeChainTest(BitcoinTestFramework):
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
        node.z_sendmany(t_addr, [{"address": z_addr, "amount": float(SHIELD)}])
        self.generatetoaddress(node, 1, t_addr)

        self.log.info("Spend change of change for %d generations", GENERATIONS)
        for generation in range(1, GENERATIONS + 1):
            node.z_sendmany(z_addr, [{"address": t_addr, "amount": float(SEND)}])
            self.generatetoaddress(node, 1, t_addr)
            notes = node.z_listunspent()
            # Every send spends the single change note and leaves one behind.
            assert_equal(len(notes), 1)
            change = Decimal(str(notes[0]["amount"]))
            assert_equal(Decimal(node.z_getbalance(z_addr)), change)
            if generation % 10 == 0:
                self.log.info("generation %d: change %s still attributed", generation, change)

        assert Decimal(node.z_getbalance(z_addr)) < SHIELD - GENERATIONS * SEND


if __name__ == "__main__":
    SaplingChangeChainTest().main()
