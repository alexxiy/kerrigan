#!/usr/bin/env python3
"""A spent Sapling note must never be resurrected, and an unspent one must come back.

Two halves, and the first is the one that matters.

1. INTEGRITY. A wallet that only ever SAW a shielded spend in a block must keep
   that note marked spent. A shielded-only transaction has no transparent input
   and no transparent output belonging to us, so AddToWalletIfInvolvingMe never
   admits it and mapWallet has no entry. Treating that absence as "the spend
   never happened" releases a note whose nullifier is on chain, and the wallet
   then reports funds that do not exist.

   This is the path the earlier regression test could not reach: it unshielded to
   an address of our own, which gave the transaction a transparent output we own,
   so it landed in mapWallet and the dangerous branch was never taken. The
   observer wallet below is the whole point. It imports the spending key and
   rescans, so the spend arrives from a block rather than being created locally.

2. RECOVERY. The original bug: a note stays flagged spent after its spending
   transaction leaves the mempool without ever being mined. Here the wallet does
   know the transaction and the chain can confirm the nullifier is absent, so the
   note is safe to release.

Requires Sapling parameters in ~/.kerrigan-params/ (run contrib/fetch-params.sh).
"""

import os
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

MATURITY = 100


class SaplingSpentNoteIntegrityTest(BitcoinTestFramework):
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
        self.integrity_observer_must_not_resurrect()
        self.recovery_unconfirmed_spend_is_released()

    def integrity_observer_must_not_resurrect(self):
        self.log.info("--- integrity: a spend seen only in a block must stay spent ---")
        node = self.nodes[0]
        t_addr = node.getnewaddress()
        self.generatetoaddress(node, MATURITY + 10, t_addr)

        z_from = node.z_getnewaddress()
        z_to = node.z_getnewaddress()
        node.z_sendmany(t_addr, [{"address": z_from, "amount": 10.0}])
        self.generatetoaddress(node, 1, t_addr)
        assert_equal(Decimal(node.z_getbalance(z_from)), Decimal("10.0"))

        # Pure z->z: no transparent input, no transparent output.
        spend_txid = node.z_sendmany(z_from, [{"address": z_to, "amount": 4.0}])
        self.generatetoaddress(node, 1, t_addr)
        node.syncwithvalidationinterfacequeue()

        key = node.z_exportkey(z_from)
        node.createwallet("observer")
        obs = node.get_wallet_rpc("observer")
        obs.z_importkey(key, "yes")
        node.syncwithvalidationinterfacequeue()

        # The observer did not create the spend, so it is not in its mapWallet.
        # That absence must NOT be read as "the spend never happened".
        try:
            obs.gettransaction(spend_txid)
            in_map_wallet = True
        except Exception:
            in_map_wallet = False
        self.log.info("observer knows the spending tx: %s (expected False)", in_map_wallet)

        expected = Decimal(obs.z_getbalance(z_from))
        self.log.info("observer balance right after import: %s", expected)

        # Drive the witness auto-check hard: new tips, then a restart replay.
        self.generatetoaddress(node, 5, t_addr)
        node.syncwithvalidationinterfacequeue()
        after_blocks = Decimal(obs.z_getbalance(z_from))
        self.log.info("observer balance after 5 blocks: %s", after_blocks)

        self.restart_node(0)
        node = self.nodes[0]
        # Wallets are not reloaded automatically after a restart.
        if "observer" not in node.listwallets():
            node.loadwallet("observer")
        obs = node.get_wallet_rpc("observer")
        self.generatetoaddress(node, 2, t_addr)
        node.syncwithvalidationinterfacequeue()
        after_restart = Decimal(obs.z_getbalance(z_from))

        self.log.info("observer balance after blocks=%s after restart=%s", after_blocks, after_restart)

        # The 10.0 note is spent on chain. It must never reappear as spendable.
        assert after_blocks < Decimal("10.0"), (
            "spent note resurrected after the witness auto-check: balance %s. The wallet is "
            "reporting funds whose nullifier is already on chain." % after_blocks
        )
        assert after_restart < Decimal("10.0"), (
            "spent note resurrected after restart: balance %s" % after_restart
        )
        assert_equal(after_blocks, expected)
        assert_equal(after_restart, expected)
        self.log.info("integrity holds: the spent note was not resurrected")

    def recovery_unconfirmed_spend_is_released(self):
        self.log.info("--- recovery: a spend that never confirmed must release its note ---")
        node = self.nodes[0]
        node.createwallet("recovery")
        w = node.get_wallet_rpc("recovery")
        t_addr = w.getnewaddress()
        self.generatetoaddress(node, MATURITY + 10, t_addr)

        z_addr = w.z_getnewaddress()
        shield = Decimal("5.0")
        w.z_sendmany(t_addr, [{"address": z_addr, "amount": float(shield)}])
        self.generatetoaddress(node, 1, t_addr)
        assert_equal(Decimal(w.z_getbalance(z_addr)), shield)

        spend_txid = w.z_sendmany(z_addr, [{"address": t_addr, "amount": 4.0}])
        node.syncwithvalidationinterfacequeue()
        assert spend_txid in node.getrawmempool()
        assert_equal(Decimal(w.z_getbalance(z_addr)), Decimal("0"))

        # Drop it from the mempool so it can never confirm.
        self.restart_node(0, extra_args=["-persistmempool=0", "-walletbroadcast=0"])
        node = self.nodes[0]
        if "recovery" not in node.listwallets():
            node.loadwallet("recovery")
        w = node.get_wallet_rpc("recovery")
        assert spend_txid not in node.getrawmempool()

        self.generatetoaddress(node, 1, t_addr)
        node.syncwithvalidationinterfacequeue()

        restored = Decimal(w.z_getbalance(z_addr))
        assert_equal(restored, shield)
        self.log.info("recovery holds: balance restored to %s", restored)


if __name__ == "__main__":
    SaplingSpentNoteIntegrityTest().main()
