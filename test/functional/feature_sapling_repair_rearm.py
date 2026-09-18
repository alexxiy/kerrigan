#!/usr/bin/env python3
"""A stranded Sapling note must come back WITHOUT restarting the daemon.

The witness auto-check is a one-shot per wallet session: the flag is set at
wallet load and cleared the first time the check runs. RepairStaleSaplingSpends
hangs off that check, so before this fix a note stranded by a spend that never
confirmed stayed stranded until someone restarted the daemon, which on a
production node can be weeks.

THE NODE IS NEVER RESTARTED IN THIS TEST, AND THAT IS THE ENTIRE POINT. A
restart re-arms the flag by itself, so a test that restarts passes whether or
not the fix works. Everything here happens in one wallet session.

Sequence:
  1. Shield, then unshield. The note is marked spent and the spend sits in the
     mempool.
  2. Advance the chain with generateblock, which does NOT pull from the mempool,
     so the spend stays unmined.
  3. Jump past the mempool expiry with setmocktime and advance again. The spend
     is evicted with reason EXPIRY.
  4. Assert the balance comes back, with no restart anywhere.

Step 0 deliberately burns the one-shot first, so that if the fix is absent the
flag is already spent and the note stays stranded. Without that, the startup
one-shot could mask the whole thing.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100


class SaplingRepairRearmTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # 1 hour is the smallest expiry the daemon accepts; mocktime jumps past it.
        self.extra_args = [["-mempoolexpiry=1", "-debug=sapling"]]

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
        shield = Decimal("5.0")
        node.z_sendmany(t_addr, [{"address": z_addr, "amount": float(shield)}])
        self.generatetoaddress(node, 1, t_addr)
        node.syncwithvalidationinterfacequeue()
        assert_equal(Decimal(node.z_getbalance(z_addr)), shield)

        # Step 0: burn the one-shot, so nothing below can be explained by it.
        self.generatetoaddress(node, 2, t_addr)
        node.syncwithvalidationinterfacequeue()
        self.log.info("one-shot auto-check has had its chance to run")

        self.log.info("Unshield, and do not let it be mined")
        spend_txid = node.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        node.syncwithvalidationinterfacequeue()
        assert spend_txid in node.getrawmempool()
        assert_equal(Decimal(node.z_getbalance(z_addr)), Decimal("0"))

        # generateblock takes an explicit tx list and ignores the mempool, so the
        # spend stays unmined while the chain moves on.
        addr = node.getnewaddress()
        node.generateblock(output=addr, transactions=[], invalid_call=False)
        node.syncwithvalidationinterfacequeue()
        assert spend_txid in node.getrawmempool(), "spend should still be pending"
        assert_equal(Decimal(node.z_getbalance(z_addr)), Decimal("0"))

        self.log.info("Jump past mempool expiry and evict it, still no restart")
        import time as _time
        node.setmocktime(int(_time.time()) + 6 * 3600)
        # The expiry sweep runs from LimitMempoolSize, which fires when a
        # transaction is accepted, not on block connect. Submit one to trigger it.
        node.sendtoaddress(addr, 0.1)
        node.syncwithvalidationinterfacequeue()
        node.generateblock(output=addr, transactions=[], invalid_call=False)
        node.syncwithvalidationinterfacequeue()

        assert spend_txid not in node.getrawmempool(), "spend should have expired out of the mempool"

        # One more tip so updatedBlockTip fires after the re-arm.
        node.generateblock(output=addr, transactions=[], invalid_call=False)
        node.syncwithvalidationinterfacequeue()

        restored = Decimal(node.z_getbalance(z_addr))
        self.log.info("balance after eviction, with no restart: %s", restored)
        assert_equal(restored, shield)
        self.log.info("the repair ran on the eviction alone, no restart needed")

        self.log.info("And the note is genuinely spendable again")
        node.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        self.generatetoaddress(node, 1, t_addr)
        node.syncwithvalidationinterfacequeue()
        assert Decimal(node.z_getbalance(z_addr)) < shield


if __name__ == "__main__":
    SaplingRepairRearmTest().main()
