// Copyright (c) 2026 The Kerrigan developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <evo/specialtx.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <sapling/sapling_address.h>
#include <sapling/sapling_init.h>
#include <sapling/sapling_tx_payload.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/sapling_keymanager.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallet {

BOOST_FIXTURE_TEST_SUITE(sapling_send_tests, TestChain100Setup)

/** Load the Sapling proving parameters, or return false when they are not installed. */
static bool LoadSaplingParams()
{
    if (sapling::IsSaplingInitialized()) return true;
    const char* home = getenv("HOME");
    if (!home || !*home) return false;
    for (const char* name : {".kerrigan-params", ".zcash-params"}) {
        const fs::path dir = fs::path(home) / name;
        const fs::path spend = dir / "sapling-spend.params";
        const fs::path output = dir / "sapling-output.params";
        if (fs::exists(spend) && fs::exists(output)) return sapling::InitSaplingParams(spend, output);
    }
    return false;
}

// The Qt Send page sends shielded payments through interfaces::Wallet, not the
// z_sendmany RPC. A memo entered there must reach the recipient's note.
BOOST_AUTO_TEST_CASE(interface_send_carries_memo)
{
    if (!LoadSaplingParams()) {
        BOOST_TEST_MESSAGE("Sapling parameters not found; skipping");
        return;
    }

    // The send spends from one transparent address, so fund that address with a
    // coinbase (the fixture's own coinbases pay to a bare public key) and mature it.
    CreateAndProcessBlock({}, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())));
    for (int i = 0; i < COINBASE_MATURITY; ++i) CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    std::shared_ptr<CWallet> wallet = CreateSyncedWallet(*m_node.chain, m_node.chainman->ActiveChain(), m_args, coinbaseKey);
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto iface = interfaces::MakeWallet(context, wallet);

    const std::string zaddr = iface->getNewSaplingAddress();
    const std::string from = EncodeDestination(PKHash(coinbaseKey.GetPubKey()));
    const std::string text = "order 12 \xe2\x9c\x93";
    const std::vector<uint8_t> memo(text.begin(), text.end());

    // A memo cannot go to a transparent address, and is at most 512 bytes.
    BOOST_CHECK_EXCEPTION(iface->saplingZSendMany(zaddr, {{from, COIN, memo}}), std::runtime_error,
        HasReason("Memo can only be sent to shielded addresses"));
    BOOST_CHECK_EXCEPTION(iface->saplingZSendMany(from, {{zaddr, COIN, std::vector<uint8_t>(513, 'a')}}), std::runtime_error,
        HasReason("Memo too long (max 512 bytes)"));

    const std::string txid = iface->saplingZSendMany(from, {{zaddr, COIN, memo}});
    // The fixture wallet does not broadcast, so read the transaction it committed.
    const CTransactionRef tx = WITH_LOCK(wallet->cs_wallet, const CWalletTx* wtx = wallet->GetWalletTx(uint256S(txid)); return wtx ? wtx->tx : nullptr);
    BOOST_REQUIRE(tx);
    const auto payload = GetTxPayload<SaplingTxPayload>(*tx, /*assert_type=*/false);
    BOOST_REQUIRE(payload);

    const auto outputs = wallet->GetSaplingKeyManager().ViewOutputs(*payload, m_node.chainman->ActiveChain().Height() + 1,
                                                                    Params().NetworkIDString(), Params().GetConsensus().SaplingHeight);
    BOOST_REQUIRE_EQUAL(outputs.size(), 1U);
    const auto& out = outputs[0];
    BOOST_CHECK(!out.outgoing);
    BOOST_CHECK(!out.change);
    BOOST_CHECK_EQUAL(EncodeSaplingAddress(out.recipient), zaddr);
    BOOST_CHECK_EQUAL(out.value, COIN);
    BOOST_CHECK(std::equal(memo.begin(), memo.end(), out.memo.begin()));
    BOOST_CHECK(std::all_of(out.memo.begin() + memo.size(), out.memo.end(), [](uint8_t b) { return b == 0; }));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
