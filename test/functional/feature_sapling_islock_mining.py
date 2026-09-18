#!/usr/bin/env python3
"""A shielded-only Sapling spend must still be mineable while InstantSend is armed.

InstantSend locks transaction *inputs*. A Sapling spend funded entirely from
notes has no transparent inputs, so no islock can ever cover it and waiting for
one is waiting forever.

The chainlock handler knew this and deliberately did not track such a
transaction. But IsTxSafeForMining() reads "not tracked" as "not old enough
yet", and the block assembler applied that check to Sapling transactions. The
two halves disagreed, so with SPORK_2 and SPORK_3 armed a z->t or z->z spend
sat in the mempool permanently and no miner would ever include it. That is
what stalled shielded withdrawals on mainnet from block 147991.

The transparent transaction here is the control: it *does* have inputs, so it
is correctly held back by the same gate. If it were mined too, the sporks
would not really be in force and the shielded assertion would prove nothing.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, force_finish_mnsync

MATURITY = 100
SPORK_KEY = "cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"


class SaplingIslockMiningTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-sporkkey=" + SPORK_KEY]]

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

        # Shield first, while the sporks are still off: the shielding tx has
        # transparent inputs and would be held back by the gate we arm below.
        self.log.info("Shield %s to %s", shield_amount, z_addr)
        node.z_sendmany(t_addr, [{"address": z_addr, "amount": float(shield_amount)}])
        self.generatetoaddress(node, 1, t_addr)
        assert_equal(Decimal(node.z_getbalance(z_addr)), shield_amount)

        # RejectConflictingBlocks() also requires masternode sync to have
        # finished, otherwise the gate silently stays off.
        force_finish_mnsync(node)

        self.log.info("Arm SPORK_2 and SPORK_3 so the miner applies the islock gate")
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        node.sporkupdate("SPORK_3_INSTANTSEND_BLOCK_FILTERING", 0)
        self.wait_until(lambda: node.spork("show")["SPORK_2_INSTANTSEND_ENABLED"] == 0)
        self.wait_until(lambda: node.spork("show")["SPORK_3_INSTANTSEND_BLOCK_FILTERING"] == 0)

        self.log.info("Control: a transparent send, which the gate SHOULD hold back")
        t_txid = node.sendtoaddress(node.getnewaddress(), 1.0)

        self.log.info("The shielded spend, which has no transparent inputs")
        z_txid = node.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        node.syncwithvalidationinterfacequeue()

        mempool = node.getrawmempool()
        assert t_txid in mempool
        assert z_txid in mempool

        self.log.info("Mine one block and see which of the two made it in")
        block_hash = self.generatetoaddress(node, 1, t_addr)[0]
        block_txs = node.getblock(block_hash)["tx"]

        assert t_txid not in block_txs, (
            "control failed: the transparent tx was mined, so the InstantSend "
            "sporks are not actually in force and this test proves nothing"
        )
        assert z_txid in block_txs, (
            "the shielded-only spend was not mined while InstantSend was armed; "
            "IsTxSafeForMining is still treating an untracked input-less tx as unsafe"
        )
        self.log.info("Shielded spend mined, transparent control correctly held back")

        node.syncwithvalidationinterfacequeue()
        assert_equal(Decimal(node.z_getbalance(z_addr)) < shield_amount, True)
        assert_equal(node.gettransaction(z_txid)["confirmations"], 1)


if __name__ == "__main__":
    SaplingIslockMiningTest().main()
