#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test z_viewtransaction.

z_viewtransaction reads the Sapling spends and outputs of one transaction with
the wallet's viewing keys. A service that matches shielded payments to orders
by memo needs it to keep working after the note is spent, from a viewing key
alone, and while the wallet is locked.

node0 pays, node1 receives, node2 holds only node1's viewing key.
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, assert_raises_rpc_error

MATURITY = 100
FEE = Decimal("0.001")
MEMO_TEXT = "order 12 ✓"


class SaplingViewTransactionTest(BitcoinTestFramework):
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

        self.log.info("A payment with a text memo, in the mempool and then confirmed")
        memo_hex = MEMO_TEXT.encode().hex()
        pay_txid = node0.z_sendmany(self.t_addr, [{"address": z1, "amount": 3, "memo": memo_hex}], 1, FEE)
        self.sync_mempools()
        view = node1.z_viewtransaction(pay_txid)
        assert_equal(view["txid"], pay_txid)
        assert_equal(view["confirmations"], 0)
        assert "blockhash" not in view
        assert_equal(view["spends"], [])
        assert_equal(len(view["outputs"]), 1)
        out = view["outputs"][0]
        assert_equal(out["outgoing"], False)
        assert_equal(out["change"], False)
        assert_equal(out["address"], z1)
        assert_equal(out["value"], Decimal("3"))
        assert_equal(out["valueSat"], 300000000)
        assert_equal(len(out["memo"]), 1024)
        assert out["memo"].startswith(memo_hex)
        assert_equal(out["memoStr"], MEMO_TEXT)

        self.mine()
        self.sync_all()
        view = node1.z_viewtransaction(pay_txid)
        assert_equal(view["confirmations"], 1)
        assert_equal(view["blockheight"], node1.getblockcount())
        assert_equal(view["blockhash"], node1.getbestblockhash())

        self.log.info("The payer cannot read it: Kerrigan Core keeps no outgoing viewing key on payments")
        assert_raises_rpc_error(-5, "no Sapling spends or outputs this wallet can read", node0.z_viewtransaction, pay_txid)

        self.log.info("Spend the note: the original payment stays readable, the spend shows the note and the change")
        t0 = node0.getnewaddress()
        spend_txid = node1.z_sendmany(z1, [{"address": t0, "amount": 1}], 1, FEE)
        self.mine()
        self.sync_all()
        view = node1.z_viewtransaction(pay_txid)
        assert_equal(view["outputs"][0]["memoStr"], MEMO_TEXT)
        view = node1.z_viewtransaction(spend_txid)
        assert_equal(view["fee"], FEE)
        assert_equal(len(view["spends"]), 1)
        spend = view["spends"][0]
        assert_equal(spend["spend"], 0)
        assert_equal(spend["txidPrev"], pay_txid)
        assert_equal(spend["outputPrev"], out["output"])
        assert_equal(spend["address"], z1)
        assert_equal(spend["valueSat"], 300000000)
        assert_equal(len(view["outputs"]), 1)
        change = view["outputs"][0]
        assert_equal(change["change"], True)
        assert_equal(change["outgoing"], False)
        assert_equal(change["value"], Decimal("3") - 1 - FEE)
        assert "memoStr" not in change  # no memo, so no text

        self.log.info("A memo that is not text has no memoStr")
        bin_txid = node0.z_sendmany(self.t_addr, [{"address": z1, "amount": 1, "memo": "f5ab"}], 1, FEE)
        self.mine()
        self.sync_all()
        out = node1.z_viewtransaction(bin_txid)["outputs"][0]
        assert out["memo"].startswith("f5ab")
        assert "memoStr" not in out

        self.log.info("A viewing-key-only wallet reads it too, with the fee from -txindex (on by default)")
        node2.z_importviewingkey(node1.z_exportviewingkey(z1), "yes", 0)
        view = node2.z_viewtransaction(pay_txid)
        assert_equal(view["fee"], FEE)
        assert_equal(view["outputs"][0]["memoStr"], MEMO_TEXT)
        assert_equal(view["outputs"][0]["address"], z1)

        self.log.info("Without -txindex the transaction is found through the block of the note it paid")
        self.restart_node(2, extra_args=["-txindex=0"])
        view = node2.z_viewtransaction(pay_txid)
        assert_equal(view["outputs"][0]["memoStr"], MEMO_TEXT)
        assert_equal(view["confirmations"], 3)
        assert "fee" not in view  # the transparent inputs cannot be found

        self.log.info("Unknown, unreadable and transparent-only transactions")
        assert_raises_rpc_error(-5, "No such transaction", node1.z_viewtransaction, "00" * 32)
        assert_raises_rpc_error(-8, "must be of length 64", node1.z_viewtransaction, "00")
        t_txid = node0.sendtoaddress(node0.getnewaddress(), 1)
        assert_raises_rpc_error(-5, "no Sapling spends or outputs", node0.z_viewtransaction, t_txid)

        self.log.info("A locked wallet reads it with its viewing keys")
        node1.encryptwallet("pass")
        node1.walletlock()
        view = node1.z_viewtransaction(pay_txid)
        assert_equal(view["outputs"][0]["memoStr"], MEMO_TEXT)
        assert_equal(node1.z_viewtransaction(spend_txid)["outputs"][0]["change"], True)


if __name__ == '__main__':
    SaplingViewTransactionTest().main()
