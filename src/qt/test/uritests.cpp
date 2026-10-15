// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <QUrl>

void URITests::uriTests()
{
    SendCoinsRecipient rv;
    QUrl uri;
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?req-dontexist="));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?dontexist="));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?label=Some Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString("Some Example Address"));
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=0.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100000);

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=1.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100100000);

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&label=Some Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Some Example"));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?message=Some Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString());

    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?message=Some Example Address", &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.label == QString());

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?req-message=Some Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    // Commas in amounts are not allowed.
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=1,000&label=Some Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=1,000.0&label=Some Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    // There are two amount specifications. The last value wins.
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&amount=200&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.amount == 20000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    // The first amount value is correct. However, the second amount value is not valid. Hence, the URI is not valid.
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    // Test label containing a question mark ('?').
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&label=?"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("?"));

    // Escape sequences are not supported.
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&label=%3F"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("%3F"));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?amount=100&label=Some Example&message=Some Example Message"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Some Example"));
    QVERIFY(rv.message == QString("Some Example Message"));

    // Verify that IS=xxx does not lead to an error (we ignore the field)
    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?IS=1"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?req-IS=1"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    // A memo (ZIP 321: base64url, padding optional) for a shielded address.
    const QString zaddr("kregsapling14hrd5sm5rxgmz8ntutuq2cuz7jef7z8df2khnrrtxuvn9mlgpmt7vqfe9hymqanja9eu2nm7qwf");
    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?amount=1&memo=b3JkZXIgMTI", &rv));
    QVERIFY(rv.address == zaddr);
    QVERIFY(rv.memo == QString("order 12"));
    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=b3JkZXIgMTI=", &rv));
    QVERIFY(rv.memo == QString("order 12"));
    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=QmVzdGVsbHVuZyDinJMgw6Q", &rv));
    QVERIFY(rv.memo == QString::fromUtf8("Bestellung \u2713 \u00e4"));
    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?req-memo=b3JkZXIgMTI", &rv));

    // Round trip through formatBitcoinURI.
    SendCoinsRecipient withMemo(zaddr, QString(), 100000000, QString());
    withMemo.memo = QString::fromUtf8("Bestellung \u2713 \u00e4");
    QVERIFY(GUIUtil::parseBitcoinURI(GUIUtil::formatBitcoinURI(withMemo), &rv));
    QVERIFY(rv.memo == withMemo.memo);
    QVERIFY(rv.amount == 100000000);

    // A transparent address cannot take a memo.
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg?memo=b3JkZXIgMTI", &rv));
    // Not base64url, binary (first byte above 0xF4), not UTF-8, or over 512 bytes.
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=b3J+ZXIgMTI", &rv));
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=9QE", &rv));
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=w6", &rv));
    // Control characters (a newline would fake a line in the send confirmation) and a byte-order mark.
    const auto b64 = [](const QByteArray& raw) { return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)); };
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=" + b64("order 12\nFrom: you"), &rv));
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=" + b64("tab\there"), &rv));
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=" + b64("\xEF\xBB\xBForder 12"), &rv));
    const QString max = QString::fromLatin1(QByteArray(512, 'a').toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const QString over = QString::fromLatin1(QByteArray(513, 'a').toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QVERIFY(GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=" + max, &rv));
    QVERIFY(!GUIUtil::parseBitcoinURI("kerrigan:" + zaddr + "?memo=" + over, &rv));
}
