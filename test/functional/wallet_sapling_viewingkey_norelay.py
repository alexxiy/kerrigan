#!/usr/bin/env python3
"""A wallet that only watches a shielded address must never rebroadcast its spends.

A full viewing key derives the nullifiers of the notes it can see, so a
watch-only wallet recognises the owner's spend when it reaches the mempool and
records it. Recognising it is fine. Announcing it is not: a node that
rebroadcasts a shielded spend tells its peers the spend is its own, which links
the viewing node to a transaction Sapling otherwise keeps unlinkable.

The owner, which built the spend, must still rebroadcast it.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os

from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100


class SaplingViewingKeyNoRelayTest(BitcoinTestFramework):
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
        owner, observer = self.nodes
        t_addr = owner.getnewaddress()
        self.generatetoaddress(owner, MATURITY + 10, t_addr)

        z_addr = owner.z_getnewaddress()
        observer.z_importviewingkey(owner.z_exportviewingkey(z_addr), "no")

        owner.z_sendmany(t_addr, [{"address": z_addr, "amount": 5.0}])
        self.generatetoaddress(owner, 1, t_addr)
        self.sync_all()

        self.log.info("The owner spends; the observer sees the spend in its mempool")
        spend_txid = owner.z_sendmany(z_addr, [{"address": t_addr, "amount": 1.0}])
        self.sync_mempools()
        for node in self.nodes:
            node.syncwithvalidationinterfacequeue()
        assert spend_txid in observer.getrawmempool()
        assert_equal(observer.gettransaction(spend_txid)["txid"], spend_txid)

        # Initialise each wallet's resend timer before moving the clock.
        for node in self.nodes:
            node.mockscheduler(60)

        # A fresh peer on each node: a tx is not re-announced to a peer that
        # already has it.
        peers = [node.add_p2p_connection(P2PTxInvStore()) for node in self.nodes]

        self.log.info("Mine a block without the spend, then pass the resend window")
        block_time = self.mocktime + 6 * 60
        for node in self.nodes:
            node.setmocktime(block_time)
        owner.generateblock(output=t_addr, transactions=[], invalid_call=False)
        self.sync_blocks()
        for node in self.nodes:
            node.syncwithvalidationinterfacequeue()
        assert spend_txid in observer.getrawmempool()

        resend_time = self.mocktime + 36 * 60 * 60
        with observer.assert_debug_log(expected_msgs=[],
                                       unexpected_msgs=[f"Submitting wtx {spend_txid}", "ResendWalletTransactions: resubmit"]):
            with owner.assert_debug_log(expected_msgs=[f"Submitting wtx {spend_txid}",
                                                       "ResendWalletTransactions: resubmit 1 unconfirmed"]):
                for node in self.nodes:
                    node.setmocktime(resend_time)
                for node in self.nodes:
                    node.mockscheduler(60)
                    node.syncwithvalidationinterfacequeue()

        self.log.info("The owner announces its own spend")
        for node in self.nodes:
            node.setmocktime(resend_time + 600)
        peers[0].wait_for_broadcast([spend_txid])

        self.log.info("The observer announces nothing")
        # The observer's peer only ever sees the tx if the observer itself pushes
        # it; give trickling the same window the owner had.
        observer.mockscheduler(60)
        observer.syncwithvalidationinterfacequeue()
        assert int(spend_txid, 16) not in peers[1].get_invs()


if __name__ == "__main__":
    SaplingViewingKeyNoRelayTest().main()
