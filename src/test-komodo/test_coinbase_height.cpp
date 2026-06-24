#include <gtest/gtest.h>
#include <cstring>

#include "primitives/transaction.h"
#include "script/script.h"
#include "core_io.h"

// Defined in komodo_bitcoind.cpp. Declared locally to avoid pulling the heavy
// komodo_bitcoind.h into the unit test.
int32_t komodo_coinbase_height(const CTransaction& coinbaseTx);

namespace TestCoinbaseHeight {

// Build a minimal coinbase-style transaction whose vin[0].scriptSig is `scriptSig`.
static CTransaction MakeCoinbaseWithScriptSig(const CScript& scriptSig)
{
    CMutableTransaction mtx;
    CTxIn vin;
    vin.prevout.SetNull();      // makes IsCoinBase() true with a single input
    vin.scriptSig = scriptSig;
    mtx.vin.push_back(vin);
    return CTransaction(mtx);
}

// Heights are written by the miner as `CScript() << nHeight` (BIP34). Decoding
// must round-trip that for every encoding form.
TEST(TestCoinbaseHeight, SyntheticEncodings)
{
    // height 0 -> OP_0 (single opcode, no length byte)
    EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(CScript() << 0)), 0);

    // heights 1..16 -> OP_1..OP_16 (single opcode, no length byte)
    for (int h = 1; h <= 16; h++)
        EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(CScript() << h)), h);

    // heights > 16 -> minimally-encoded little-endian CScriptNum data push,
    // including the byte-width boundaries and the 4-byte (INT32) maximum.
    const int heights[] = { 17, 100, 127, 128, 255, 256, 65535, 65536,
                            16777215, 16777216, 100000, 1000000, 3000000,
                            2100000000, 2147483647 };
    for (int h : heights)
        EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(CScript() << h)), h);
}

// Malformed / hostile scriptSigs must return -1, never read out of bounds, never throw.
TEST(TestCoinbaseHeight, Malformed)
{
    // empty scriptSig -> GetOp() finds nothing
    EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(CScript())), -1);

    // no inputs at all
    {
        CMutableTransaction mtx;
        EXPECT_EQ(komodo_coinbase_height(CTransaction(mtx)), -1);
    }

    // truncated push: declares 5 bytes but only 2 follow -> GetOp() fails
    {
        CScript s;
        s.push_back(0x05);
        s.push_back(0xff); s.push_back(0xff);
        EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(s)), -1);
    }

    // overlong number: 5-byte push exceeds CScriptNum's 4-byte max -> rejected
    {
        CScript s;
        s.push_back(0x05);
        for (int i = 0; i < 5; i++) s.push_back(0x7f);
        EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(s)), -1);
    }

    // first item is a non-push opcode -> not a height
    EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(CScript() << OP_RETURN)), -1);

    // a negative CScriptNum (sign bit set) is not a valid height
    {
        CScript s;
        s.push_back(0x01);
        s.push_back(0x81);  // -1 in CScriptNum sign-magnitude
        EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(s)), -1);
    }
}

// Block 0 (genesis): the KMD/Zcash genesis predates BIP34. Its coinbase scriptSig is
//   CScript() << 520617983 << CScriptNum(4) << "Zcash0b9c..."  (see chainparams.cpp)
// so the first push decodes to 520617983 (0x1F07FFFF), NOT 0. komodo_coinbase_height is
// a pure, context-free BIP34 decoder, so it faithfully returns whatever is encoded.
// The genesis height (0) is the caller's concern: komodo_block2height takes it from the
// block index, which is exactly why it prefers the index over the coinbase.
TEST(TestCoinbaseHeight, GenesisBlock0)
{
    const char* pszTimestamp = "Zcash0b9c4eef8b7cc417ee5001e3500984b6fea35683a7cac141a043c42064835d34";
    CScript genesisScriptSig = CScript() << 520617983 << CScriptNum(4)
        << std::vector<unsigned char>((const unsigned char*)pszTimestamp,
                                       (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    EXPECT_EQ(komodo_coinbase_height(MakeCoinbaseWithScriptSig(genesisScriptSig)), 520617983);
}

// Real KMD mainnet coinbase transactions (raw hex from the chain).
// Height 1 and 12 exercise the OP_N path; 100000/1000000/3000000 the CScriptNum path.
// Source: https://kmdexplorer.gleec.com/  (insight-api-komodo /rawtx/<coinbase-txid>)
TEST(TestCoinbaseHeight, RealKMDCoinbases)
{
    struct Vector { int32_t height; const char* rawtx; };
    const Vector vectors[] = {
        { 1,
          "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff03510101ffffffff010000c16ff286230023210260b255850cfd4a20f5f97eafbbd05df9b56203f7d480744dd1c8aec8d17cdc01ac00000000" },
        { 12,
          "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff035c0103ffffffff0100a3e11100000000232102b8c743960257bb60a52181dce0fbbbfdccfa47bacc2fc74ec6eddb0d22e46eceac00000000" },
        { 100000,
          "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0603a086010102ffffffff0200a3e111000000002321026b49dd3923b78a592c1b475f208e23698d3f085c4c3b4906a59faf659fd9530bac0000000000000000986a4c9550841c48582300000007f7d304481d772d7be8a608000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
        { 1000000,
          "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff060340420f0102ffffffff0188b6e11100000000232103396ac453b3f23e20f30d4793c5b8ab6ded6993242df4f09fd91eb9a4f8aede84ac9b13935b" },
        { 3000000,
          "0400008085202f89010000000000000000000000000000000000000000000000000000000000000000ffffffff0503c0c62d00ffffffff015066e211000000001976a914bd239d36363bcccf2f8c243f7f736d71a5e675b588ac00000000000000000000000000000000000000" },
    };

    for (const Vector& v : vectors) {
        CTransaction tx;
        ASSERT_TRUE(DecodeHexTx(tx, v.rawtx)) << "decode failed for height " << v.height;
        EXPECT_TRUE(tx.IsCoinBase()) << "not a coinbase for height " << v.height;
        EXPECT_EQ(komodo_coinbase_height(tx), v.height) << "wrong height for " << v.height;
    }
}

} // namespace TestCoinbaseHeight
