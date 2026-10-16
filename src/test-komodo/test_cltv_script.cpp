#include <gtest/gtest.h>

#include "core_io.h"
#include "key.h"
#include "key_io.h"
#include "keystore.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script_ext.h"
#include "script/sign.h"
#include "script/standard.h"
#include "utilstrencodings.h"
#include "wallet/wallet_ismine.h"

#include <univalue.h>

// RPC's serializer is separate from ScriptPubKeyToUniv; exercise both paths.
void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex);

namespace {

CScript FromHex(const char* hex)
{
    const auto bytes = ParseHex(hex);
    return CScript(bytes.begin(), bytes.end());
}

const char* const FAKE_CLTV =
    "4c01b175a82035174c3d2af25de3dfbd0bdbf964337119c132dc0dd251681f93fa0c297e57ea88"
    "61616161616161616161616161616161"
    "61616161616161616161616161616161"
    "61616161616161"
    "1976a914f1dce4182fce875748c4986b240ff7d7bc3fffb088ac";

struct PrefixCase {
    const char* hex;
    int64_t value;
};

const PrefixCase ACCEPTED[] = {
    {"00b175", 0},
    {"0101b175", 1},       // Minimal number, non-minimal push (OP_1 is excluded).
    {"0110b175", 16},
    {"0111b175", 17},
    {"017fb175", 127},
    {"028000b175", 128},
    {"02b100b175", 177},   // A CLTV byte inside data is harmless with a real CLTV after it.
    {"02ff00b175", 255},
    {"020001b175", 256},
    {"020102b175", 513},   // Little endian, not big endian.
    {"03ffff00b175", 65535},
    {"03000001b175", 65536},
    {"0400000001b175", 16777216},
    {"04ffffff7fb175", 2147483647LL},
    {"050000008000b175", 2147483648LL}, // Must not clamp through getint().
    {"05ffffffff00b175", 4294967295LL},
    {"050000000001b175", 4294967296LL}, // Recognition does not check tx.nLockTime.
    {"05ffffffff7fb175", 549755813887LL},
    {"0100b175", 0},       // Non-minimal numeric encodings are intentional.
    {"020100b175", 1},
    {"050100000000b175", 1},
    {"0180b175", 0},       // Negative zero is accepted and decoded as zero.
    {"020080b175", 0},
    {"050000000080b175", 0},
};

void ExpectRejected(const CScript& script)
{
    int64_t value = 123456789;
    EXPECT_FALSE(script.IsCheckLockTimeVerify(&value));
    EXPECT_EQ(123456789, value);
    EXPECT_FALSE(script.IsCheckLockTimeVerify(nullptr));
    EXPECT_FALSE(script.IsCheckLockTimeVerify());
}

class CLTVScript : public ::testing::Test {
protected:
    CBasicKeyStore keystore;
    CKey key;
    CScript p2pkh;

    void SetUp() override
    {
        key.MakeNewKey(true);
        ASSERT_TRUE(keystore.AddKey(key));
        p2pkh = GetScriptForDestination(key.GetPubKey().GetID());
    }

    void ExpectDestination(const CScript& script)
    {
        const CTxDestination expected = key.GetPubKey().GetID();
        CTxDestination destination;
        ASSERT_TRUE(ExtractDestination(script, destination));
        EXPECT_TRUE(destination == expected);
        txnouttype type = TX_NONSTANDARD;
        std::vector<CTxDestination> destinations;
        int required = -1;
        ASSERT_TRUE(ExtractDestinations(script, type, destinations, required));
        EXPECT_EQ(TX_PUBKEYHASH, type);
        ASSERT_EQ(1U, destinations.size());
        EXPECT_TRUE(destinations[0] == expected);
        EXPECT_EQ(1, required);
        EXPECT_EQ(ISMINE_SPENDABLE, IsMine(keystore, script));
    }

    void ExpectNonstandard(const CScript& script)
    {
        CTxDestination destination;
        EXPECT_FALSE(ExtractDestination(script, destination));
        txnouttype type = TX_PUBKEYHASH;
        std::vector<CTxDestination> destinations(1, key.GetPubKey().GetID());
        int required = -1;
        EXPECT_FALSE(ExtractDestinations(script, type, destinations, required));
        EXPECT_EQ(TX_NONSTANDARD, type);
        EXPECT_TRUE(destinations.empty());
        EXPECT_EQ(ISMINE_NO, IsMine(keystore, script));
        SignatureData signature;
        EXPECT_FALSE(ProduceSignature(DummySignatureCreator(&keystore), script, signature, 0));
        EXPECT_TRUE(signature.scriptSig.empty());
    }
};

TEST_F(CLTVScript, DirectPushNumbersAndConsumerOffsets)
{
    for (const auto& test : ACCEPTED) {
        SCOPED_TRACE(test.hex);
        const CScript prefix = FromHex(test.hex);
        const CScript script = prefix + p2pkh;
        int64_t value = -1;
        ASSERT_TRUE(script.IsCheckLockTimeVerify(&value));
        EXPECT_EQ(test.value, value);
        EXPECT_TRUE(script.IsCheckLockTimeVerify(nullptr));
        EXPECT_TRUE(script.IsCheckLockTimeVerify());

        // Pin the contract used by extraction, signing and wallet ownership.
        const size_t offset = static_cast<size_t>(script[0]) + 3;
        ASSERT_EQ(prefix.size(), offset);
        EXPECT_TRUE(CScript(script.begin() + offset, script.end()) == p2pkh);
        ExpectDestination(script);
    }
}

TEST_F(CLTVScript, NegativeNumbersAndSixByteNumbersAreRejected)
{
    for (const char* hex : {"0181b175", "01ffb175", "028080b175",
                           "020180b175", "050100000080b175", "05ffffffffffb175",
                           "4fb175", "06010000000000b175", "06000000000000b175"}) {
        SCOPED_TRACE(hex);
        const CScript script = FromHex(hex) + p2pkh;
        ExpectRejected(script);
        ExpectNonstandard(script);
    }
}

TEST_F(CLTVScript, AlternatePushOpcodesAreDeliberatelyUnsupported)
{
    // A valid numeric constant is insufficient: callers require opcode == size.
    for (int op = OP_1; op <= OP_16; ++op) {
        SCOPED_TRACE(op);
        const CScript script = (CScript() << static_cast<opcodetype>(op)
            << OP_CHECKLOCKTIMEVERIFY << OP_DROP) + p2pkh;
        ExpectRejected(script);
        ExpectNonstandard(script);
    }
    for (const char* hex : {"4c0111b175", "4d010011b175", "4e0100000011b175",
                           "4c00b175", "4d0000b175", "4e00000000b175"}) {
        SCOPED_TRACE(hex);
        const CScript script = FromHex(hex) + p2pkh;
        ExpectRejected(script);
        ExpectNonstandard(script);
    }
}

TEST_F(CLTVScript, TruncatedInstructionsAndMissingDrop)
{
    // Every truncation of an otherwise valid prefix, including a completed push
    // at end(), must fail without indexing beyond the script.
    for (const auto& test : ACCEPTED) {
        const CScript prefix = FromHex(test.hex);
        for (size_t size = 0; size < prefix.size(); ++size) {
            SCOPED_TRACE(test.hex);
            SCOPED_TRACE(size);
            ExpectRejected(CScript(prefix.begin(), prefix.begin() + size));
        }
    }
    for (const char* hex : {"4c", "4c01", "4d", "4d01", "4d0100", "4e",
                           "4e01", "4e0100", "4e010000", "4e01000000",
                           "4effffffff", "051122", "0111b14c", "0111b14c01",
                           "0111b161", "0111b176", "01116175", "011101b175",
                           "0111b10175", "01b175", "4c01b175", "4d0100b175",
                           "4e01000000b175", "61b175", "76b175", "6ab175",
                           "50b175", "ffb175"}) {
        SCOPED_TRACE(hex);
        ExpectRejected(FromHex(hex));
    }
}

TEST_F(CLTVScript, EmptyRemainderIsOnlyAPrefix)
{
    for (const auto& test : ACCEPTED) {
        SCOPED_TRACE(test.hex);
        const CScript prefix = FromHex(test.hex);
        EXPECT_TRUE(prefix.IsCheckLockTimeVerify());
        ExpectNonstandard(prefix);
    }
    // Recognition deliberately does not parse or validate the remainder.
    const CScript malformedTail = FromHex("0111b1754c");
    EXPECT_TRUE(malformedTail.IsCheckLockTimeVerify());
    ExpectNonstandard(malformedTail);
}

TEST_F(CLTVScript, OriginalFakeCLTVIsNotP2PKH)
{
    const CScript original = FromHex(FAKE_CLTV);
    ASSERT_EQ(104U, original.size());
    ASSERT_EQ(0x19, original[78]);
    ExpectRejected(original);
    ExpectNonstandard(original);

    // Put a key actually owned by this keystore inside the final data push.
    const CScript ownedFake = CScript(original.begin(), original.begin() + 79) + p2pkh;
    ExpectRejected(ownedFake);
    ExpectNonstandard(ownedFake);

    for (const CScript& script : {original, ownedFake}) {
        for (auto serialize : {ScriptPubKeyToUniv, ScriptPubKeyToJSON}) {
            UniValue json(UniValue::VOBJ);
            serialize(script, json, true);
            EXPECT_EQ("nonstandard", find_value(json, "type").get_str());
            EXPECT_TRUE(find_value(json, "addresses").isNull());
            EXPECT_TRUE(find_value(json, "reqSigs").isNull());
            EXPECT_EQ(HexStr(script.begin(), script.end()), find_value(json, "hex").get_str());
            EXPECT_EQ(ScriptToAsmStr(script), find_value(json, "asm").get_str());
        }
    }
}

TEST_F(CLTVScript, OrdinaryAndPrefixedP2PKHSerializeCorrectly)
{
    EXPECT_FALSE(p2pkh.IsCheckLockTimeVerify());
    for (const CScript& script : {p2pkh, FromHex("0111b175") + p2pkh}) {
        ExpectDestination(script);
        for (auto serialize : {ScriptPubKeyToUniv, ScriptPubKeyToJSON}) {
            UniValue json(UniValue::VOBJ);
            serialize(script, json, true);
            EXPECT_EQ("pubkeyhash", find_value(json, "type").get_str());
            EXPECT_EQ(1, find_value(json, "reqSigs").get_int());
            ASSERT_EQ(1U, find_value(json, "addresses").size());
            EXPECT_EQ(EncodeDestination(key.GetPubKey().GetID()),
                      find_value(json, "addresses")[0].get_str());
        }
    }
}

TEST_F(CLTVScript, SigningUsesCorrectRemainderAndFullScript)
{
    // Exercise SignStep through real signatures and subsequent verification.
    for (uint32_t locktime : {0U, 17U, 128U, 32768U, 8388608U, 2147483648U, 4294967295U}) {
        SCOPED_TRACE(locktime);
        const CScript script = (CScript() << CScriptNum::serialize(locktime)
            << OP_CHECKLOCKTIMEVERIFY << OP_DROP) + p2pkh;
        CMutableTransaction tx;
        tx.nLockTime = locktime;
        tx.vin.resize(1);
        tx.vin[0].nSequence = 0xfffffffe;
        tx.vout.emplace_back(1, p2pkh);
        ASSERT_TRUE(SignSignature(keystore, script, tx, 0, 2, SIGHASH_ALL, 0));
        ScriptError error;
        EXPECT_TRUE(VerifyScript(tx.vin[0].scriptSig, script, STANDARD_SCRIPT_VERIFY_FLAGS,
                                MutableTransactionSignatureChecker(&tx, 0, 2), 0, &error));
        EXPECT_EQ(SCRIPT_ERR_OK, error);

        // A recognized prefix does not mean its transaction condition is met.
        tx.vin[0].nSequence = 0xffffffff;
        EXPECT_FALSE(VerifyScript(tx.vin[0].scriptSig, script, STANDARD_SCRIPT_VERIFY_FLAGS,
                                 MutableTransactionSignatureChecker(&tx, 0, 2), 0, &error));
        EXPECT_EQ(SCRIPT_ERR_UNSATISFIED_LOCKTIME, error);
    }
}

TEST_F(CLTVScript, NonminimalNumbersAreRecognizedButSigningPolicyStillApplies)
{
    // EvalScript currently maps scriptnum_error to UNKNOWN_ERROR, whereas a
    // non-minimal push is reported directly as MINIMALDATA.
    const struct { const char* hex; ScriptError error; } cases[] = {
        {"020100b175", SCRIPT_ERR_UNKNOWN_ERROR},
        {"0180b175", SCRIPT_ERR_UNKNOWN_ERROR},
        {"0101b175", SCRIPT_ERR_MINIMALDATA},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.hex);
        const CScript script = FromHex(test.hex) + p2pkh;
        ASSERT_TRUE(script.IsCheckLockTimeVerify());
        CMutableTransaction tx;
        tx.nLockTime = 1;
        tx.vin.resize(1);
        tx.vin[0].nSequence = 0xfffffffe;
        tx.vout.emplace_back(1, p2pkh);
        // SignStep can find the key; ProduceSignature's final policy check fails.
        EXPECT_FALSE(SignSignature(keystore, script, tx, 0, 2, SIGHASH_ALL, 0));
        ASSERT_FALSE(tx.vin[0].scriptSig.empty());
        ScriptError error;
        EXPECT_TRUE(VerifyScript(tx.vin[0].scriptSig, script,
                                STANDARD_SCRIPT_VERIFY_FLAGS & ~SCRIPT_VERIFY_MINIMALDATA,
                                MutableTransactionSignatureChecker(&tx, 0, 2), 0, &error));
        EXPECT_EQ(SCRIPT_ERR_OK, error);
        EXPECT_FALSE(VerifyScript(tx.vin[0].scriptSig, script, STANDARD_SCRIPT_VERIFY_FLAGS,
                                 MutableTransactionSignatureChecker(&tx, 0, 2), 0, &error));
        EXPECT_EQ(test.error, error);
    }
}

TEST_F(CLTVScript, MultiplePrefixesPreserveExistingConsumerBehavior)
{
    // ExtractDestinations recurses; single-destination extraction, IsMine and
    // SignStep strip only one prefix. This change does not broaden those APIs.
    CScript script = p2pkh;
    for (int i = 0; i < 32; ++i)
        script = FromHex("0111b175") + script;
    EXPECT_TRUE(script.IsCheckLockTimeVerify());
    txnouttype type;
    std::vector<CTxDestination> destinations;
    int required = -1;
    ASSERT_TRUE(ExtractDestinations(script, type, destinations, required));
    EXPECT_EQ(TX_PUBKEYHASH, type);
    ASSERT_EQ(1U, destinations.size());
    EXPECT_TRUE(destinations[0] == CTxDestination(key.GetPubKey().GetID()));
    EXPECT_EQ(1, required);
    CTxDestination destination;
    EXPECT_FALSE(ExtractDestination(script, destination));
    EXPECT_EQ(ISMINE_NO, IsMine(keystore, script));
    SignatureData signature;
    EXPECT_FALSE(ProduceSignature(DummySignatureCreator(&keystore), script, signature, 0));
    EXPECT_TRUE(signature.scriptSig.empty());
}

TEST_F(CLTVScript, ScriptExtTimelockBuilderRemainsCompatible)
{
    for (int64_t locktime : {1LL, 16LL, 17LL, 2147483648LL, 4294967295LL}) {
        CScriptExt script;
        script.TimeLockSpend(key.GetPubKey().GetID(), locktime);
        int64_t decoded = -1;
        ASSERT_TRUE(script.IsCheckLockTimeVerify(&decoded));
        EXPECT_EQ(locktime, decoded);
        ExpectDestination(script);
        CMutableTransaction tx;
        tx.vout.emplace_back(1, script);
        CTxDestination destination;
        ASSERT_TRUE(CScriptExt::ExtractVoutDestination(CTransaction(tx), 0, destination));
        EXPECT_TRUE(destination == CTxDestination(key.GetPubKey().GetID()));
    }
}

TEST_F(CLTVScript, TimelockedCoinbaseMetadataUsesTheSameParser)
{
    for (const auto& test : ACCEPTED) {
        SCOPED_TRACE(test.hex);
        const CScript redeem = FromHex(test.hex) + p2pkh;
        CScriptExt metadata;
        metadata.OpReturnScript(redeem, OPRETTYPE_TIMELOCK);
        CMutableTransaction tx;
        tx.vin.resize(1); // Null prevout makes this a coinbase.
        tx.vout.emplace_back(1, GetScriptForDestination(CScriptID(redeem)));
        tx.vout.emplace_back(0, metadata);
        const CTransaction coinbase(tx);
        ASSERT_TRUE(coinbase.IsCoinBase());
        EXPECT_EQ(test.value, coinbase.UnlockTime(0));
        CTxDestination destination;
        ASSERT_TRUE(CScriptExt::ExtractVoutDestination(coinbase, 0, destination));
        EXPECT_TRUE(destination == CTxDestination(key.GetPubKey().GetID()));
    }

    for (const CScript& redeem : {FromHex(FAKE_CLTV), FromHex("0181b175") + p2pkh,
                                  FromHex("0111b161") + p2pkh}) {
        CScriptExt metadata;
        metadata.OpReturnScript(redeem, OPRETTYPE_TIMELOCK);
        CMutableTransaction tx;
        tx.vin.resize(1);
        const CScriptID scriptID(redeem);
        tx.vout.emplace_back(1, GetScriptForDestination(scriptID));
        tx.vout.emplace_back(0, metadata);
        const CTransaction coinbase(tx);
        EXPECT_EQ(0, coinbase.UnlockTime(0));
        CTxDestination destination;
        ASSERT_TRUE(CScriptExt::ExtractVoutDestination(coinbase, 0, destination));
        // Invalid timelock metadata must not expose the embedded P2PKH key.
        EXPECT_TRUE(destination == CTxDestination(scriptID));
    }
}

} // namespace
