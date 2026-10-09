#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Call the shielded RPCs through kerrigan-cli.

kerrigan-cli and the Qt console send an argument as JSON only when its
(method, position) is in vRPCConvertParams; anything else arrives as a string.
The z_* calls were missing from that table, so z_sendmany failed with
"JSON value of type string is not of expected type array" and the numbers of
z_listunspent arrived as strings (GitHub issue #11). This test sends every one
of those arguments through the real kerrigan-cli binary, by position and by name.
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100
MEMO_HEX = "3132"  # "12", as in the issue


class KerriganCliSaplingTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=True)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_cli()
        params_dir = os.path.expanduser("~/.kerrigan-params")
        for name in ("sapling-spend.params", "sapling-output.params"):
            if not os.path.exists(os.path.join(params_dir, name)):
                raise SkipTest("Sapling parameters not found. Run contrib/fetch-params.sh first.")

    def run_test(self):
        node0, node1 = self.nodes
        cli = node0.cli

        t_addr = node0.getnewaddress()
        self.generatetoaddress(node0, MATURITY + 10, t_addr)
        z_addr = node0.z_getnewaddress()

        self.log.info("z_sendmany by position: amounts array, minconf and fee")
        txid = cli.z_sendmany(t_addr, [{"address": z_addr, "amount": 1, "memo": MEMO_HEX}], 1, Decimal("0.001"))
        assert_equal(len(txid), 64)
        self.generatetoaddress(node0, 1, t_addr)

        self.log.info("z_listunspent by position: minconf, maxconf and an address array")
        notes = cli.z_listunspent(1, 9999999, [z_addr])
        assert_equal(len(notes), 1)
        assert_equal(Decimal(str(notes[0]["amount"])), Decimal("1"))
        assert notes[0]["memo"].startswith(MEMO_HEX)
        assert_equal(cli.z_listunspent(2, 9999999, [z_addr]), [])

        self.log.info("z_getbalance with minconf")
        assert_equal(Decimal(str(cli.z_getbalance(z_addr, 1))), Decimal("1"))
        assert_equal(Decimal(str(cli.z_getbalance(z_addr, 5))), Decimal("0"))

        self.log.info("z_sendmany and z_listunspent by name")
        z_addr2 = node0.z_getnewaddress()
        cli.z_sendmany(fromaddress=t_addr, amounts=[{"address": z_addr2, "amount": 2}], minconf=1, fee=Decimal("0.001"))
        self.generatetoaddress(node0, 1, t_addr)
        notes = cli.z_listunspent(minconf=1, maxconf=9999999, addresses=[z_addr2])
        assert_equal(len(notes), 1)
        assert_equal(Decimal(str(notes[0]["amount"])), Decimal("2"))
        assert_equal(Decimal(str(cli.z_getbalance(address=z_addr2, minconf=1))), Decimal("2"))
        self.sync_all()

        self.log.info("z_importviewingkey and z_importkey: rescan stays a string, startHeight is a number")
        node1.cli.z_importviewingkey(node0.z_exportviewingkey(z_addr), "yes", 0)
        node1.cli.z_importkey(node0.z_exportkey(z_addr2), "yes", 0)
        # Both addresses are diversified addresses of the same key, so node1 finds both notes.
        assert_equal(Decimal(str(node1.cli.z_getbalance())), Decimal("3"))
        assert_equal(sorted(n["address"] for n in node1.cli.z_listunspent(1, 9999999)), sorted([z_addr, z_addr2]))

        self.log.info("z_rebuildsaplingwitnesses with from_height")
        node0.cli.z_rebuildsaplingwitnesses(0)

        self.log.info("getblocksubsidy and getdronelist with a height")
        assert_equal(cli.getblocksubsidy(1), node0.getblocksubsidy(1))
        cli.getdronelist(1)


if __name__ == '__main__':
    KerriganCliSaplingTest().main()
