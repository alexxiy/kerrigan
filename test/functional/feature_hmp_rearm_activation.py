#!/usr/bin/env python3
# Copyright (c) 2026 The Kerrigan developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_hmp_rearm_activation.py

v1.3.2 HMP re-arm (CCbTx v6 / HMP_REARM) activation straddle.

CheckCbTx applies a TWO-SIDED gate keyed on the block height: at/after
nHMPRearmHeight the coinbase MUST be version 6 (HMP_REARM), and below it version
6 is REJECTED. This test mines across a regtest re-arm height and asserts:
  * the coinbase version is < 6 just below the height and exactly 6 at/after it,
    UNCONDITIONALLY (even with no seals or commitments to embed);
  * the v6 payload carries the new fields (a set minerIdentitySig, and the two
    parallel proof/sig vectors);
  * gethmpinfo reports rearm_active flipping false -> true at the height;
  * mining continues across the boundary with no stall (the whole v6
    emission + validation path works end to end).
The CheckCbTx two-sided gate itself (reject <v6 at/after, reject >=v6 before) is
covered at unit level by hmp_tests/checkcbtx_v6_gating-style assertions.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

REARM = 130


class HMPRearmActivation(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-dip3params=2:2",
            "-testactivationheight=v20@2",
            "-testactivationheight=hmp_rearm@%d" % REARM,
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def cbtx_version(self, height):
        bh = self.nodes[0].getblockhash(height)
        cb = self.nodes[0].getblock(bh, 2)["tx"][0]
        return (cb.get("cbTx") or {}).get("version")

    def run_test(self):
        node = self.nodes[0]
        addr = node.getnewaddress()

        self.log.info("Mine to just below the re-arm height (%d)" % REARM)
        self.generatetoaddress(node, REARM - 1 - node.getblockcount(), addr)
        assert_equal(node.getblockcount(), REARM - 1)
        assert_equal(node.gethmpinfo()["rearm_active"], False)
        pre_v = self.cbtx_version(REARM - 1)
        self.log.info("cbTx version just below re-arm: %s" % pre_v)
        assert pre_v is not None and pre_v < 6, "expected pre-rearm coinbase version < 6"

        self.log.info("Cross the re-arm height")
        self.generatetoaddress(node, 5, addr)
        assert_greater_than(node.getblockcount(), REARM)  # chain advanced, no stall
        assert_equal(node.gethmpinfo()["rearm_active"], True)

        for h in (REARM, REARM + 1, node.getblockcount()):
            assert_equal(self.cbtx_version(h), 6)
        self.log.info("cbTx version == 6 from the re-arm height onward")

        # v6 payload fields are populated even with no seals/commitments.
        tip_cb = node.getblock(node.getbestblockhash(), 2)["tx"][0]["cbTx"]
        assert tip_cb.get("minerIdentitySig"), "v6 coinbase missing minerIdentitySig"
        assert "vSealSignerVRFProofs" in tip_cb
        assert "vCommitmentSigs" in tip_cb

        self.log.info("HMP re-arm activation straddle: OK")


if __name__ == "__main__":
    HMPRearmActivation().main()
