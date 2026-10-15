#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that shielded receives are wallet transactions.

A transaction that pays the wallet only through a Sapling output used to be
unknown to gettransaction ("Invalid or non-wallet transaction id"),
listtransactions and listsinceblock. It is now a wallet transaction, and each
note it paid to one of the wallet's own (external) addresses is listed as a
"receive" entry with "shielded": true.

node0 pays, node1 receives, node2 holds only node1's viewing key.
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100
FEE = Decimal("0.001")


def shielded_entries(entries, txid):
    return [e for e in entries if e["txid"] == txid and e.get("shielded")]


class SaplingTxListTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        params_dir = os.path.expanduser("~/.kerrigan-params")
        for name in ("sapling-spend.params", "sapling-output.params"):
            if not os.path.exists(os.path.join(params_dir, name)):
                raise SkipTest("Sapling parameters not found. Run contrib/fetch-params.sh first.")

    def mine(self, n=1):
        self.generatetoaddress(self.nodes[0], n, self.t_addr)

    def run_test(self):
        node0, node1, node2 = self.nodes
        self.t_addr = node0.getnewaddress()
        self.mine(MATURITY + 10)
        z1 = node1.z_getnewaddress()
        start_hash = node1.getbestblockhash()

        self.log.info("A shielded receive in the mempool is a wallet transaction")
        txid = node0.z_sendmany(self.t_addr, [{"address": z1, "amount": 3}], 1, FEE)
        self.sync_mempools()
        tx = node1.gettransaction(txid)
        assert_equal(tx["confirmations"], 0)
        assert_equal(tx["amount"], 0)  # the transparent amount: none of it is ours
        assert_equal(len(tx["details"]), 1)
        detail = tx["details"][0]
        assert_equal(detail["address"], z1)
        assert_equal(detail["category"], "receive")
        assert_equal(detail["amount"], Decimal("3"))
        assert_equal(detail["shielded"], True)
        assert "vout" not in detail
        listed = shielded_entries(node1.listtransactions(), txid)
        assert_equal(len(listed), 1)
        assert_equal(listed[0]["outindex"], detail["outindex"])
        assert_equal(listed[0]["confirmations"], 0)

        self.log.info("Once mined it has confirmations and a block hash")
        self.mine()
        self.sync_all()
        tx = node1.gettransaction(txid)
        assert_equal(tx["confirmations"], 1)
        assert_equal(tx["blockhash"], node1.getbestblockhash())
        assert_equal(len(shielded_entries(node1.listsinceblock(start_hash)["transactions"], txid)), 1)
        assert_equal(node1.getbalance(), 0)  # transparent balances are unchanged

        self.log.info("A shielded receive seen first in a block")
        self.disconnect_nodes(0, 1)
        block_txid = node0.z_sendmany(self.t_addr, [{"address": z1, "amount": 2}], 1, FEE)
        self.generatetoaddress(node0, 1, self.t_addr, sync_fun=self.no_op)
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes[0:2])
        tx = node1.gettransaction(block_txid)
        assert_equal(tx["confirmations"], 1)
        assert_equal(len(shielded_entries(node1.listtransactions(), block_txid)), 1)

        self.log.info("Spending a note: the change is not listed as a receive")
        spend_txid = node1.z_sendmany(z1, [{"address": node0.getnewaddress(), "amount": 1}], 1, FEE)
        self.sync_mempools()
        self.mine()
        self.sync_all()
        assert_equal(node1.gettransaction(spend_txid)["confirmations"], 1)
        assert_equal(shielded_entries(node1.listtransactions("*", 100), spend_txid), [])

        self.log.info("The transactions survive a restart")
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.sync_all()
        assert_equal(node1.gettransaction(txid)["confirmations"], 3)
        assert_equal(node1.gettransaction(block_txid)["confirmations"], 2)

        self.log.info("A reorg returns the transaction to the mempool and back")
        block = node1.gettransaction(block_txid)["blockhash"]
        node1.invalidateblock(block)
        assert_equal(node1.gettransaction(block_txid)["confirmations"], 0)
        node1.reconsiderblock(block)
        assert_equal(node1.gettransaction(block_txid)["confirmations"], 2)

        self.log.info("A shielded payment someone else sent is never resubmitted; our own spend is")
        received_txid = node0.z_sendmany(self.t_addr, [{"address": z1, "amount": 1}], 1, FEE)
        own_txid = node1.z_sendmany(z1, [{"address": node0.getnewaddress(), "amount": 1}], 1, FEE)
        self.sync_mempools()
        # Restart with an empty mempool and no peers: whatever is in it after
        # startup was resubmitted by the wallet itself.
        self.restart_node(1, extra_args=["-persistmempool=0"])
        mempool = node1.getrawmempool()
        assert own_txid in mempool
        assert received_txid not in mempool
        assert_equal(node1.gettransaction(received_txid)["confirmations"], 0)  # still a wallet transaction
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.mine()
        self.sync_all()

        self.log.info("A viewing-key-only wallet finds them on rescan, as watch-only")
        node2.z_importviewingkey(node1.z_exportviewingkey(z1), "yes", 0)
        tx = node2.gettransaction(txid, True)
        assert_equal(tx["details"][0]["involvesWatchonly"], True)
        assert_equal(len(shielded_entries(node2.listtransactions("*", 100, 0, True), txid)), 1)
        assert_equal(shielded_entries(node2.listtransactions("*", 100, 0, False), txid), [])


if __name__ == '__main__':
    SaplingTxListTest().main()
