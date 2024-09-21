/******************************************************************************
/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include <stdlib.h>
#include <list>
#include <algorithm>
#include <chrono>

#include "main.h"
#include "txdb.h"
#include "komodo_defs.h"
#include "CCMarmara.h"
#include "key_io.h"
//#include <signal.h>
 /*
  Marmara CC is for the MARMARA project

  'B' initial data for credit loop
  vins normal
  vout0 request to senderpk (issuer)

  'R' request for credit issuance 
  vins normal
  vout0 request to senderpk (endorser)

  'I' check issuance
  vin0 request from MARMARA_REQUEST
  vins1+ normal
  vout0 baton to 1st receiverpk
  vout1 marker to Marmara so all issuances can be tracked (spent when loop is closed)

  'T' check transfer to endorser
  vin0 request from MARMARA_REQUEST
  vin1 baton from MARMARA_ISSUE/MARMARA_TRANSFER
  vins2+ normal
  vout0 baton to next receiverpk (following the unspent baton back to original is the credit loop)

  'S' check settlement
  vin0 MARMARA_ISSUE marker
  vin1 baton
  vins CC utxos from credit loop

  'D' default/partial payment in the settlement
  //TODO: should we implement several partial settlements in case of too many vins?

  'A' activated funds
  'F' activated funds with 3x stake advantage  
  'N' initially activated funds at h=2 to fill all 64 segids on first blocks

  'C' marmara coinbase
  'E' marmara coinbase with 3x stake advantage

  'L' lock in loop last vout opret

  'K' locked-in-loop cc vout opret with the pubkey which locked his funds in this vout 

  'O' unlocked (released to normals from activated) coins opret

 */


const bool CHECK_ONLY_CCOPRET = true;

// credit loop data structure allowing to store data from different LCL tx oprets
struct SMarmaraCreditLoopOpret {
    bool hasCreateOpret;
    bool hasIssuanceOpret;
    bool hasSettlementOpret;

    uint8_t lastfuncid;

    uint8_t autoSettlement;
    uint8_t autoInsurance;

    // create tx data:
    CAmount amount;  // loop amount
    int32_t matures; // check maturing height
    std::string currency;  // currently MARMARA

    // issuer data:
    int32_t disputeExpiresHeight;
    uint8_t escrowOn;
    CAmount blockageAmount;

    // last issuer/endorser/receiver data:
    uint256 createtxid;
    CPubKey pk;             // always the last pk in opret
    int32_t avalCount;      // only for issuer/endorser

    // settlement data:
    CAmount remaining;

    uint8_t version;

    // init default values:
    SMarmaraCreditLoopOpret() {
        hasCreateOpret = false;
        hasIssuanceOpret = false;
        hasSettlementOpret = false;

        lastfuncid = 0;

        amount = 0LL;
        matures = 0;
        autoSettlement = 1;
        autoInsurance = 1;

        createtxid = zeroid;
        disputeExpiresHeight = 0;
        avalCount = 0;
        escrowOn = false;
        blockageAmount = 0LL;

        remaining = 0L;
        version = 0;
    }
};


// Classes to check opret by calling CheckOpret member func for two cases:
// 1) the opret in cc vout data is checked first and considered primary
// 2) if it is not required to check only cc opret, the opret in the last vout is checked second and considered secondary
// returns the opret and pubkey from the opret

class CMarmaraOpretCheckerBase {
public:
    bool checkOnlyCC;
    virtual bool CheckOpret(const CScript &spk, CPubKey &opretpk) const = 0;
};

#define SET_TO_STRING(T, paramset) [](const std::set<T> &set) { std::string res; for (auto const &e : set) res += std::to_string(e) + " "; return res; }(paramset)

// checks if opret for activated coins, returns pk from opret
class CMarmaraActivatedOpretChecker : public CMarmaraOpretCheckerBase
{
public:
    CMarmaraActivatedOpretChecker() { checkOnlyCC = true; }   // only the cc opret allowed now
                                                        // CActivatedOpretChecker(bool onlyCC) { checkOnlyCC = onlyCC; }
    bool CheckOpret(const CScript &spk, CPubKey &opretpk) const
    {
        int32_t ht, unlockht;

        return MarmaraDecodeCoinbaseOpret(spk, opretpk, ht, unlockht) != 0;
    }
};

// checks if opret for lock-in-loop coins, returns pk from opret
class CMarmaraLockInLoopOpretChecker : public CMarmaraOpretCheckerBase
{
public:
    //CMarmaraLockInLoopOpretChecker(uint8_t _checkVersion) { checkOnlyCC = false; checkVersion = _checkVersion; }
    CMarmaraLockInLoopOpretChecker(bool onlyCC, uint8_t _checkVersion) { checkOnlyCC = onlyCC; checkVersion = _checkVersion; }
    bool CheckOpret(const CScript &spk, CPubKey &opretpk) const
    {
        struct SMarmaraCreditLoopOpret loopData;

        uint8_t funcid = MarmaraDecodeLoopOpret(spk, loopData, checkVersion);
        if (funcid != 0) {
            opretpk = loopData.pk;
            return true;
        }
        return false;
    }
private:
    uint8_t checkVersion;
};

static bool skipBadLoop(const uint256 &refbatontxid);
static bool fixBadSettle(const uint256 &settletxid);

// helper functions for rpc calls

/* see similar funcs below
int64_t IsMarmaravout(struct CCcontract_info *cp, const CTransaction& tx, int32_t v)
{
    char destaddr[KOMODO_ADDRESS_BUFSIZE];
    if (tx.vout[v].scriptPubKey.IsPayToCryptoCondition() != 0)
    {
        if (Getscriptaddress(destaddr, tx.vout[v].scriptPubKey) && strcmp(destaddr, cp->unspendableCCaddr) == 0)
            return(tx.vout[v].nValue);
    }
    return(0);
}*/

// Get randomized within range [3 month...2 year] using ind as seed(?)
/* not used now
int32_t MarmaraRandomize(uint32_t ind)
{
    uint64_t val64; uint32_t val, range = (MARMARA_MAXLOCK - MARMARA_MINLOCK);
    val64 = komodo_block_prg(ind);
    val = (uint32_t)(val64 >> 32);
    val ^= (uint32_t)val64;
    return((val % range) + MARMARA_MINLOCK);
}
*/

// get random but fixed for the height param unlock height within 3 month..2 year interval  -- discontinued
// now always returns maxheight
int32_t MarmaraUnlockht(int32_t height)
{
    /*  uint32_t ind = height / MARMARA_GROUPSIZE;
    height = (height / MARMARA_GROUPSIZE) * MARMARA_GROUPSIZE;
    return(height + MarmaraRandomize(ind)); */
    return MARMARA_V2LOCKHEIGHT;
}

// get exactly it like in komodo_staked()
static int32_t get_next_height()
{
    CBlockIndex *tipindex = chainActive.Tip();
    if (tipindex == NULL)
        return(0);
    return tipindex->GetHeight() + 1;
}

// decode activated coin opreturn
uint8_t MarmaraDecodeCoinbaseOpretExt(const CScript &scriptPubKey, uint8_t &version, CPubKey &pk, int32_t &height, int32_t &unlockht, int32_t &matureht)
{
    vscript_t vopret;
    GetOpReturnData(scriptPubKey, vopret);

    if (vopret.size() >= 3)
    {
        uint8_t evalcode, funcid;
        if (vopret[0] == EVAL_MARMARA)
        {
            if (IsFuncidOneOf(vopret[1], MARMARA_ACTIVATED_FUNCIDS))
            {
                if (vopret[2] >= 1 && vopret[2] <= 2)  // check version 
                {
                    if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> funcid; ss >> version; ss >> pk;
                        if (version == 1) {
                            ss >> height; ss >> unlockht;
                        }
                        if (version == 2) {
                            ss >> matureht; 
                        }) != 0)
                    {
                        return(vopret[1]);
                    }
                    else
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "opret unmarshal error for funcid=" << (char)vopret[1] << std::endl);
                }
                else
                    LOGSTREAMFN("marmara", CCLOG_INFO, stream << "incorrect marmara activated or coinbase opret version=" << (char)vopret[2] << std::endl);
            }
            else
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "not marmara activated or coinbase funcid=" << (char)vopret[1] << std::endl);
        }
        else
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "not marmara opret, evalcode=" << (int)vopret[0] << std::endl);
    }
    else {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "bad marmara opret, vopret.size()=" << vopret.size() << std::endl);
        //raise(SIGINT);
    }
    return(0);
}

uint8_t MarmaraDecodeCoinbaseOpret(const CScript &scriptPubKey, CPubKey &pk, int32_t &height, int32_t &unlockht)
{
    uint8_t version;
    int32_t matureht;

    return MarmaraDecodeCoinbaseOpretExt(scriptPubKey, version, pk, height, unlockht, matureht);
}

// encode activated coin opreturn
CScript MarmaraEncodeCoinbaseOpretExt(uint8_t version, uint8_t funcid, const CPubKey &pk, int32_t ht, int32_t matureht)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;

    if (version == 1)
    {
        int32_t unlockht = MarmaraUnlockht(ht);
        opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << pk << ht << unlockht;);
    }
    else
    {
        opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << pk; 
                if (version == 2) ss << matureht; 
            );
    }

    return(opret);
}

CScript MarmaraEncodeCoinbaseOpret(uint8_t funcid, const CPubKey &pk, int32_t ht)
{
    return MarmaraEncodeCoinbaseOpretExt(1, funcid, pk, ht, 0);
}

// encode lock-in-loop tx opret functions:

CScript MarmaraEncodeLoopCreateOpret(uint8_t version, CPubKey senderpk, int64_t amount, int32_t matures, std::string currency)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_CREATELOOP; // create tx (initial request tx)

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << senderpk << amount << matures << currency);
    return(opret);
}

CScript MarmaraEncodeLoopIssuerOpret(uint8_t version, uint256 createtxid, CPubKey receiverpk, uint8_t autoSettlement, uint8_t autoInsurance, int32_t avalCount, int32_t disputeExpiresHeight, uint8_t escrowOn, CAmount blockageAmount)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_ISSUE; // issuance tx

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << createtxid << receiverpk << autoSettlement << autoInsurance << avalCount << disputeExpiresHeight << escrowOn << blockageAmount);
    return(opret);
}

CScript MarmaraEncodeLoopRequestOpret(uint8_t version, uint256 createtxid, CPubKey senderpk)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_REQUEST; // request tx

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << createtxid << senderpk);
    return(opret);
}

CScript MarmaraEncodeLoopTransferOpret(uint8_t version, uint256 createtxid, CPubKey receiverpk, int32_t avalCount)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_TRANSFER; // transfer tx

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << createtxid << receiverpk << avalCount);
    return(opret);
}

CScript MarmaraEncodeLoopCCVoutOpret(uint256 createtxid, CPubKey senderpk)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_LOCKED; // opret in cc 1of2 lock-in-loop vout
    uint8_t version = MARMARA_OPRET_VERSION;

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << createtxid << senderpk);
    return(opret);
}

CScript MarmaraEncodeLoopSettlementOpret(uint8_t version, bool isSuccess, uint256 createtxid, CPubKey pk, CAmount remaining)
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = isSuccess ? MARMARA_SETTLE : MARMARA_SETTLE_PARTIAL;

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version << createtxid << pk << remaining);
    return(opret);
}

// decode different lock-in-loop oprets, update the loopData
uint8_t MarmaraDecodeLoopOpret(const CScript scriptPubKey, struct SMarmaraCreditLoopOpret &loopData, uint8_t checkVersion)
{
    vscript_t vopret;

    GetOpReturnData(scriptPubKey, vopret);
    if (vopret.size() >= 3)
    {
        uint8_t evalcode = vopret.begin()[0];
        uint8_t funcid = vopret.begin()[1];
        uint8_t version = vopret.begin()[2];

        if (evalcode == EVAL_MARMARA)   // check limits
        {
            bool found = false;
            if (funcid == MARMARA_CREATELOOP) {  // createtx
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.pk; ss >> loopData.amount; ss >> loopData.matures; ss >> loopData.currency)) {
                    loopData.hasCreateOpret = true;
                    found = true;
                }
            }
            else if (funcid == MARMARA_ISSUE) {
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.createtxid; ss >> loopData.pk; ss >> loopData.autoSettlement; ss >> loopData.autoInsurance; ss >> loopData.avalCount >> loopData.disputeExpiresHeight >> loopData.escrowOn >> loopData.blockageAmount)) {
                    loopData.hasIssuanceOpret = true;
                    found = true;
                }
            }
            else if (funcid == MARMARA_REQUEST) {
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.createtxid; ss >> loopData.pk)) {
                    found = true;
                }
            }
            else if (funcid == MARMARA_TRANSFER) {
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.createtxid; ss >> loopData.pk; ss >> loopData.avalCount)) {
                    found = true;
                }
            }
            else if (funcid == MARMARA_LOCKED) {
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.createtxid; ss >> loopData.pk)) {
                    found = true;
                }
            }
            else if (funcid == MARMARA_SETTLE || funcid == MARMARA_SETTLE_PARTIAL) {
                if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> loopData.lastfuncid; ss >> loopData.version; ss >> loopData.createtxid; ss >> loopData.pk >> loopData.remaining)) {
                    loopData.hasSettlementOpret = true;
                    found = true;
                }
            }
            // getting here from any E_UNMARSHAL error too

            if (!found) {
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "cannot parse loop opret: not my funcid=" << (int)funcid << " or bad opret format=" << HexStr(vopret) << std::endl);
                return 0;
            }
           
            if (checkVersion != MARMARA_OPRET_VERSION_ANY && version != checkVersion)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "unsupported opret version=" << (int)version << std::endl);
                return 0;
            }
            return funcid;  
        }
        else
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "not marmara opret, evalcode=" << (int)evalcode << std::endl);
    }
    else
        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "opret too small=" << HexStr(vopret) << std::endl);

    return(0);
}

// decode release coin opreturn
uint8_t MarmaraDecodeReleaseOpret(const CScript &scriptPubKey, uint8_t &version, uint8_t checkVersion)
{
    vscript_t vopret;
    GetOpReturnData(scriptPubKey, vopret);

    if (vopret.size() >= 3)
    {
        uint8_t evalcode, funcid;
        if (vopret[0] == EVAL_MARMARA)
        {
            if (IsFuncidOneOf(vopret[1], { MARMARA_RELEASE }))
            {
                if (checkVersion == MARMARA_OPRET_VERSION_ANY || vopret[2] == checkVersion)  // check version 
                {
                    if (E_UNMARSHAL(vopret, ss >> evalcode; ss >> funcid; ss >> version) != 0)
                    {
                        return(vopret[1]);
                    }
                }
            }
        }
    }
    return(0);
}

CScript MarmaraEncodeReleaseOpret()
{
    CScript opret;
    uint8_t evalcode = EVAL_MARMARA;
    uint8_t funcid = MARMARA_RELEASE; 
    uint8_t version = MARMARA_OPRET_VERSION;

    opret << OP_RETURN << E_MARSHAL(ss << evalcode << funcid << version);
    return(opret);
}

static CTxOut MakeMarmaraCC1of2voutOpret(CAmount amount, const CPubKey &pk2, const CScript &opret)
{
    vscript_t vopret;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, 0);

    GetOpReturnData(opret, vopret);
    if (!vopret.empty()) {
        std::vector< vscript_t > vData{ vopret };    // add mypk to vout to identify who has locked coins in the credit loop
        return MakeCC1of2vout(EVAL_MARMARA, amount, Marmarapk, pk2, &vData);
    }
    else
        return MakeCC1of2vout(EVAL_MARMARA, amount, Marmarapk, pk2, NULL);
}

bool MyGetCCopret(const CScript &scriptPubKey, CScript &opret)
{
    std::vector<std::vector<unsigned char>> vParams;
    CScript dummy; 

    if (scriptPubKey.IsPayToCryptoCondition(&dummy, vParams) != 0)
    {
        if (vParams.size() >= 1)  // allow more data after cc opret
        {
            //uint8_t version;
            //uint8_t evalCode;
            //uint8_t m, n;
            std::vector< vscript_t > vData;

            // parse vParams[0] as script
            CScript inScript(vParams[0].begin(), vParams[0].end());
            CScript::const_iterator pc = inScript.begin();
            inScript.GetPushedData(pc, vData);

            if (vData.size() > 1 && vData[0].size() == 4) // first vector is 4-byte header
            {
                //vscript_t vopret(vParams[0].begin() + 6, vParams[0].end());
                opret << OP_RETURN << vData[1];  // return vDatap[1] as cc opret
                return true;
            }
        }
    }
    return false;
}

static bool GetCCOpReturnData(const CScript &spk, CScript &opret)
{
    CScript dummy;
    std::vector< vscript_t > vParams;

    return MyGetCCopret(spk, opret);

    // get cc opret
    /* if (spk.IsPayToCryptoCondition(&dummy, vParams))
    {
        if (vParams.size() > 0)
        {
            COptCCParams p = COptCCParams(vParams[0]);  // does not work as we has removed pubkeys
            if (p.vData.size() > 0)
            {
                opret << OP_RETURN << p.vData[0]; // reconstruct opret 
                return true;
            }
        } 
    }*/
    return false;
}

// add mined coins
int64_t AddMarmaraCoinbases(struct CCcontract_info *cp, CMutableTransaction &mtx, int32_t firstheight, CPubKey poolpk, int32_t maxinputs)
{
    char coinaddr[KOMODO_ADDRESS_BUFSIZE];
    int64_t nValue, totalinputs = 0;
    uint256 txid, hashBlock;
    CTransaction vintx;
    int32_t unlockht, ht, vout, unlocks, n = 0;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;

    CPubKey Marmarapk = GetUnspendable(cp, NULL);
    GetCCaddress1of2(cp, coinaddr, Marmarapk, poolpk);
    SetCCunspents(unspentOutputs, coinaddr, true);
    unlocks = MarmaraUnlockht(firstheight);

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " check coinaddr=" << coinaddr << std::endl);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        txid = it->first.txhash;
        vout = (int32_t)it->first.index;
        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " txid=" << txid.GetHex() << " vout=" << vout << std::endl);
        if (myGetTransaction(txid, vintx, hashBlock) != 0)
        {
            if (vintx.IsCoinBase() != 0 && vintx.vout.size() == 2 && vintx.vout[1].nValue == 0)
            {
                CPubKey pk;
                if (MarmaraDecodeCoinbaseOpret(vintx.vout[1].scriptPubKey, pk, ht, unlockht) == MARMARA_COINBASE && unlockht == unlocks && pk == poolpk && ht >= firstheight)
                {
                    if ((nValue = vintx.vout[vout].nValue) > 0 && myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, vout) == 0)
                    {
                        if (maxinputs != 0)
                            mtx.vin.push_back(CTxIn(txid, vout, CScript()));
                        nValue = it->second.satoshis;
                        totalinputs += nValue;
                        n++;
                        if (maxinputs > 0 && n >= maxinputs)
                            break;
                    }
                    else
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "tx in mempool or vout not positive, nValue=" << nValue << std::endl);
                }
                else
                    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "decode error unlockht=" << unlockht << " vs unlocks=" << unlocks << " is-pool-pk=" << (pk == poolpk) << std::endl);
            }
            else
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "not coinbase" << std::endl);
        }
        else
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error getting tx=" << txid.GetHex() << std::endl);
    }
    return(totalinputs);
}


// returns first normal vin pubkey
static CPubKey GetFirstNormalInputPubKey(const CTransaction &tx)
{
    for (const auto &vin : tx.vin) 
    {    
        if (!IsCCInput(vin.scriptSig)) 
        {
             CTransaction vintx;
            uint256 hashBlock;
            if (myGetTransaction(vin.prevout.hash, vintx, hashBlock))
            {
                std::vector<vuint8_t> vSolutions;
                txnouttype whichType;
                if (Solver(vintx.vout[vin.prevout.n].scriptPubKey, whichType, vSolutions)) 
                {
                    if (whichType == TX_PUBKEY) {
                        if (vSolutions.size() >= 1)
                            return CPubKey(vSolutions[0]); // vSolutions[0] is pubkey
                    }
                    else if (whichType == TX_PUBKEYHASH)    {
                        std::vector<vuint8_t> vData;
                        CScriptBase::const_iterator pc = vin.scriptSig.begin();
                        vin.scriptSig.GetPushedData(pc, vData);
                        if (vData.size() >= 2)
                            return CPubKey(vData[1]);  // vData[0] is signature, vData[1] is pubkey
                    }
                    // TODO: TX_SCRIPTHASH
                }
            }
        }
    }
    return CPubKey();
}

// checks either of two options for tx:
// tx has cc vin for the evalcode
static bool tx_has_my_cc_vin(struct CCcontract_info *cp, const CTransaction &tx)
{
    for (auto const &vin : tx.vin)
        if (cp->ismyvin(vin.scriptSig))
            return true;

    return false;
}

// check if this is a activated vout:
static bool activated_vout_matches_pk_in_opret(const CTransaction &tx, int32_t nvout, const CScript &opret)
{
    CPubKey pk;
    int32_t h, unlockh;

    MarmaraDecodeCoinbaseOpret(opret, pk, h, unlockh);
    if (tx.vout[nvout] == MakeMarmaraCC1of2voutOpret(tx.vout[nvout].nValue, pk, opret))
        return true;
    else
        return false;
}

// check if this is a LCL vout:
static bool vout_matches_createtxid_in_opret(const CTransaction &tx, int32_t nvout, const CScript &opret)
{
    struct SMarmaraCreditLoopOpret loopData;
    MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_ANY);

    CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);

    if (tx.vout[nvout] == MakeMarmaraCC1of2voutOpret(tx.vout[nvout].nValue, createtxidPk, opret))
        return true;
    else
        return false;
}


// calls checker first for the cc vout opret then for the last vout opret
static bool get_either_opret(CMarmaraOpretCheckerBase *opretChecker, const CTransaction &tx, int32_t nvout, CScript &opretOut, CPubKey &opretpk)
{
    CScript opret;
    bool isccopret = false, opretok = false;

    if (!opretChecker)
        return false;

    if (nvout < 0 || nvout >= tx.vout.size())
        return false;

    // first check cc opret
    if (GetCCOpReturnData(tx.vout[nvout].scriptPubKey, opret))
    {
        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "ccopret=" << opret.ToString() << std::endl);
        if (opretChecker->CheckOpret(opret, opretpk))
        {
            isccopret = true;
            opretok = true;
            opretOut = opret;
        }
    }

    // then check opret in the last vout:
    if (!opretChecker->checkOnlyCC && !opretok)   // if needed opret was not found in cc vout then check opret in the back of vouts
    {
        if (nvout < tx.vout.size()-1) {   // there might be opret in the back
            opret = tx.vout.back().scriptPubKey;
            if (opretChecker->CheckOpret(opret, opretpk))
            {
                isccopret = false;
                opretok = true;
                opretOut = opret;
            }
        }
    }

    // print opret evalcode and funcid for debug logging:
    vscript_t vprintopret;
    uint8_t funcid = 0, evalcode = 0;
    if (GetOpReturnData(opret, vprintopret) && vprintopret.size() >= 2)
    {
        evalcode = vprintopret.begin()[0];
        funcid = vprintopret.begin()[1];
    }
    LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << " opret eval=" << (int)evalcode << " funcid=" << (char)(funcid ? funcid : ' ') << " isccopret=" << isccopret << std::endl);
    return opretok;
}

// checks if tx vout is valid activated coins:
// - activated opret is okay
// - vin txns are funded from marmara cc inputs (this means they were validated while added to the chain) 
// - or vin txns are self-funded from normal inputs
// returns the pubkey from the opret
bool IsMarmaraActivatedVout(const CTransaction &tx, int32_t nvout, CPubKey &pk_in_opret, uint256 &dummytxid)
{
    CMarmaraActivatedOpretChecker activatedOpretChecker;
    CScript opret;

    if (nvout < 0 || nvout >= tx.vout.size())
        return false;

    // this check considers 2 cases:
    // first if opret is in the cc vout data
    // second if opret is in the last vout
    if (get_either_opret(&activatedOpretChecker, tx, nvout, opret, pk_in_opret))
    {
        // check opret pk matches vout
        if (activated_vout_matches_pk_in_opret(tx, nvout, opret))
        {
            // we allow activated coins funded from any normal inputs
            // so this check is removed:
            /* struct CCcontract_info *cp, C;
            cp = CCinit(&C, EVAL_MARMARA);

            // if activated opret is okay
            // check that vin txns have cc inputs (means they were checked by the pos or cc marmara validation code)
            // this rule is disabled: `or tx is self-funded from normal inputs (marmaralock)`
            // or tx is coinbase with activated opret
            if (!tx_has_my_cc_vin(cp, tx) && TotalPubkeyNormalInputs(tx, pk_in_opret) == 0 && !tx.IsCoinBase())
            {
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "vintx=" << tx.GetHash().GetHex() << " has no marmara cc inputs or self-funding normal inputs" << std::endl);
                return false;
            }*/

            // vout is okay
            return true;
        }
        else
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "tx=" << tx.GetHash().GetHex() << " pubkey in opreturn does not match vout" << std::endl);
            return false;
        }
    }
    return false;
}


// checks if tx vout is valid locked-in-loop coins
// - activated opret is okay
// - vin txns are funded from marmara cc inputs (this means they were validated while added to the chain)
// returns the pubkey from the opret

bool IsMarmaraLockedInLoopVout(const CTransaction &tx, int32_t nvout, CPubKey &pk_in_opret,  uint256 &createtxid)
{
    CMarmaraLockInLoopOpretChecker lclOpretChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);  // for cc vout data ver is always 1
    CScript opret;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, NULL);

    if (nvout < 0 || nvout >= tx.vout.size())
        return false;

    // this check considers 2 cases:
    // first if opret is in the cc vout data
    // second if opret is in the last vout
    if (get_either_opret(&lclOpretChecker, tx, nvout, opret, pk_in_opret))
    {
        SMarmaraCreditLoopOpret loopData;
        uint8_t funcid;

        if ((funcid = MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_ANY)) != MARMARA_LOCKED)
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "tx=" << tx.GetHash().GetHex() << " nvout=" << nvout << " has incorrect funcid=" << (int)funcid << std::endl);
            return false;
        }

        // check opret pk matches vout
        if (vout_matches_createtxid_in_opret(tx, nvout, opret))
        {
            struct CCcontract_info *cp, C;
            cp = CCinit(&C, EVAL_MARMARA);

            // if opret is okay
            // check that vintxns have cc inputs
            if (!tx_has_my_cc_vin(cp, tx))
            {
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "tx=" << tx.GetHash().GetHex() << " has no marmara cc inputs" << std::endl);
                return false;
            }

            // vout is okay
            createtxid = loopData.createtxid;
            return true;
        }
        else
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "tx=" << tx.GetHash().GetHex() << " pubkey in opreturn does not match vout" << std::endl);
            return false;
        }
    }
    return false;
}

// add activated or locked-in-loop coins from 1of2 address 
// for lock-in-loop mypk not checked, so all locked-in-loop utxos for an address are added:
template <typename IsMarmaraVoutT>
int64_t AddMarmaraCCInputs(IsMarmaraVoutT IsMarmaraVout, CMutableTransaction &mtx, std::vector<CPubKey> &pubkeys, const char *unspentaddr, CAmount amount, int32_t maxinputs)
{
    CAmount totalinputs = 0, totaladded = 0;
    
    if (maxinputs > CC_MAXVINS)
        maxinputs = CC_MAXVINS;

    // threshold not used any more
    /*if (maxinputs > 0)
    threshold = total / maxinputs;
    else
    threshold = total;*/

    std::vector<CC_utxo> utxos;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
    SetCCunspents(unspentOutputs, (char*)unspentaddr, true);

    if (amount != 0 && unspentOutputs.size() > 0)  // if amount == 0 only calc total
    {
        utxos.reserve(unspentOutputs.size());
        if (utxos.capacity() == 0)
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "not enough memory to load utxos" << std::endl);
            return -1;
        }
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "adding utxos from addr=" << unspentaddr << " total=" << amount << std::endl);

    // add all utxos from cc addr:
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        uint256 txid = it->first.txhash;
        int32_t nvout = (int32_t)it->first.index;
        uint256 hashBlock;
        CTransaction tx;

        //TODO: decide on threshold usage, could be side effects like 'insufficient funds' error
        //if (it->second.satoshis < threshold)
        //    continue;

        // check if vin might be already added to mtx:
        if (std::find_if(mtx.vin.begin(), mtx.vin.end(), [&](CTxIn v) {return (v.prevout.hash == txid && v.prevout.n == nvout); }) != mtx.vin.end()) {
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "skipping already added txid=" << txid.GetHex() << " nvout=" << nvout << " satoshis=" << it->second.satoshis << std::endl);
            continue;
        }

        bool isSpentInMempool = false;
        if (myGetTransaction(txid, tx, hashBlock) && 
            tx.vout.size() > 0 &&
            tx.vout[nvout].scriptPubKey.IsPayToCryptoCondition() &&
            !(isSpentInMempool = myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, nvout)))
        {
            CPubKey opretpk;
            uint256 createtxid;
            //CScript opret;
            std::vector< vscript_t > vParams;

            // picks up either activated or LCL vouts
            if (IsMarmaraVout(tx, nvout, opretpk, createtxid))      //if (CheckEitherOpRet(opretChecker, tx, nvout, opret, senderpk))
            {
                char utxoaddr[KOMODO_ADDRESS_BUFSIZE];

                Getscriptaddress(utxoaddr, tx.vout[nvout].scriptPubKey);
                if (strcmp(unspentaddr, utxoaddr) == 0)  // check if the real vout address matches the index address (as another key could be used in the addressindex)
                {
                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found good vintx for addr=" << unspentaddr << " txid=" << txid.GetHex() << " nvout=" << nvout << " satoshis=" << it->second.satoshis << std::endl);

                    if (amount != 0)
                    {
                        CC_utxo ccutxo{ txid, it->second.satoshis, nvout };
                        utxos.push_back(ccutxo);
                        pubkeys.push_back(opretpk); // add endorsers pubkeys
                    }
                    totalinputs += it->second.satoshis;
                }
                else
                    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "incorrect index addr=" << unspentaddr << " vs utxoaddr=" << utxoaddr << " txid=" << txid.GetHex() << std::endl);
            }
            else
                LOGSTREAMFN("marmara", CCLOG_INFO, stream << "addr=" << unspentaddr << " txid=" << txid.GetHex() << " nvout=" << nvout << " IsMarmaraVout returned false, skipping vout" << std::endl);
        }
        else
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "skipping txid=" << txid.GetHex() << " nvout=" << nvout << " satoshis=" << it->second.satoshis  << " isSpentInMempool=" << isSpentInMempool << std::endl);
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "for addr=" << unspentaddr << " found total=" << totalinputs << std::endl);
    if (amount == 0)
        return totalinputs;

    // add best selected utxos:
    CAmount remains = amount;
    while (utxos.size() > 0)
    {
        int64_t below = 0, above = 0;
        int32_t abovei = -1, belowi = -1, ind = -1;

        if (CC_vinselect(&abovei, &above, &belowi, &below, utxos.data(), utxos.size(), remains) < 0)
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error finding unspent" << " remains=" << remains << " amount=" << amount << " utxos.size()=" << utxos.size() << std::endl);
            return(0);
        }
        if (abovei >= 0) // best is 'above'
            ind = abovei;
        else if (belowi >= 0)  // second try is 'below'
            ind = belowi;
        else
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error finding unspent" << " remains=" << remains << " amount=" << amount << " abovei=" << abovei << " belowi=" << belowi << " ind=" << " utxos.size()=" << utxos.size() << ind << std::endl);
            return(0);
        }

        mtx.vin.push_back(CTxIn(utxos[ind].txid, utxos[ind].vout, CScript()));
        totaladded += utxos[ind].nValue;
        remains -= utxos[ind].nValue;

        // remove used utxo[ind]:
        utxos[ind] = utxos.back();
        utxos.pop_back();

        if (totaladded >= amount) // found the requested amount
            break;
        if (mtx.vin.size() >= maxinputs)  // reached maxinputs
            break;
    }

//    if (totaladded < amount)  // why do we need this?
//        return 0;

    return totaladded;
}



// finds the creation txid from the loop tx opret or 
// return itself if it is the request tx
static int32_t get_create_txid(uint256 &createtxid, uint256 txid, uint8_t checkVersion)
{
    CTransaction tx; 
    uint256 hashBlock; 
  
    createtxid = zeroid;
    if (myGetTransaction(txid, tx, hashBlock) != 0 /*&& !hashBlock.IsNull()*/ && tx.vout.size() > 1)  // might be called from validation code, so non-locking version
    {
        uint8_t funcid;
        struct SMarmaraCreditLoopOpret loopData;

        if ((funcid = MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, checkVersion)) == MARMARA_ISSUE || funcid == MARMARA_TRANSFER || funcid == MARMARA_REQUEST ) {
            createtxid = loopData.createtxid;
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream  << "found for funcid=" << (char)funcid << " createtxid=" << createtxid.GetHex() << std::endl);
            return(0);
        }
        else if (funcid == MARMARA_CREATELOOP)
        {
            if (createtxid == zeroid)  // TODO: maybe this is not needed 
                createtxid = txid;
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream  << "found for funcid=" << (char)funcid << " createtxid=" << createtxid.GetHex() << std::endl);
            return(0);
        }
    }
    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "could not get createtxid for txid=" << txid.GetHex() << " tx.vout.size()=" << tx.vout.size() << std::endl);
    return(-1);
}

// starting from any baton txid, finds the latest yet unspent batontxid 
// adds createtxid MARMARA_CREATELOOP in creditloop vector (only if there are other txns in the loop)
// finds all the baton txids starting from the createtx (1+ in creditloop vector), apart from the latest baton txid
// returns the number of txns marked with the baton
// DO NOT USE this function from the validation code when the validated tx is the last baton because it is not guaranteed that it is properly updated in the spent index and coin cache!
int32_t MarmaraGetbatontxid(std::vector<uint256> &creditloop, uint256 &batontxid, uint256 querytxid)
{
    uint256 createtxid; 
    int64_t value; 
    int32_t vini, height, n = 0;
    const int32_t USE_MEMPOOL = 0;
    const int32_t DO_LOCK = 1;
    
    uint256 txid = querytxid;
    batontxid = zeroid;
    if (get_create_txid(createtxid, txid, MARMARA_OPRET_VERSION_ANY) == 0) // retrieve the initial creation txid
    {
        uint256 spenttxid;
        txid = createtxid;
        //fprintf(stderr,"%s txid.%s -> createtxid %s\n", __func__, txid.GetHex().c_str(),createtxid.GetHex().c_str());

        while (CCgetspenttxid(spenttxid, vini, height, txid, MARMARA_BATON_VOUT) == 0)  // while the current baton is spent
        {
            CTransaction spentTx;
            uint256 hashBlock;
            creditloop.push_back(txid);
            // fprintf(stderr,"%d: %s\n",n,txid.GetHex().c_str());
            n++;

            if ((value = CCgettxout(spenttxid, MARMARA_BATON_VOUT, USE_MEMPOOL, DO_LOCK)) == MARMARA_BATON_AMOUNT)  //check if the baton value is unspent yet - this is the last baton
            {
                batontxid = spenttxid;
                //fprintf(stderr,"%s got baton %s %.8f\n", __func__, batontxid.GetHex().c_str(),(double)value/COIN);
                return n;
            }
            else if (value > 0)
            {
                batontxid = spenttxid;
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream  << "n=" << n << " found and will use false baton=" << batontxid.GetHex() << " vout=" << MARMARA_BATON_VOUT << " value=" << value << std::endl);
                return n;
            }  

            // (TODO: get funcid and check for I and T?)
            txid = spenttxid;
        }      
        if (n == 0)     
            return 0;   // empty loop
        else
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "n != 0 return bad loop querytxid=" << querytxid.GetHex() << " n=" << n << std::endl);
            return -1;  //bad loop
        }
    }
    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "could not get createtxid for querytxid=" << querytxid.GetHex() << std::endl);
    return -1;
}


// starting from any baton txid, finds the latest yet unspent batontxid 
// adds createtxid MARMARA_CREATELOOP in creditloop vector (only if there are other txns in the loop)
// finds all the baton txids starting from the createtx (1+ in creditloop vector), apart from the latest baton txid
// returns the number of txns marked with the baton
// DO NOT USE this function from the validation code because it is not guaranteed that the validated tx is properly updated in the spent index and coin cache!
static int32_t get_loop_endorsers_number(uint256 &createtxid, uint256 prevtxid, uint8_t checkVersion)
{
    CTransaction tx;
    uint256 hashBlock;

    createtxid = zeroid;
    if (myGetTransaction(prevtxid, tx, hashBlock) /* && !hashBlock.IsNull()*/ && tx.vout.size() > 1)  // will be called from validation code, so non-locking version
    {
        struct SMarmaraCreditLoopOpret loopData;

        uint8_t funcid = MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, checkVersion);

        if (funcid == MARMARA_CREATELOOP) {
            createtxid = tx.GetHash();
            return 0;
        }
        else if (funcid == MARMARA_ISSUE)
        {
            createtxid = loopData.createtxid;
            return 1;
        }
        else if (funcid == MARMARA_TRANSFER)
        {
            createtxid = loopData.createtxid;
            // calc endorsers vouts:
            int32_t n = 0;
            for (int32_t ivout = 0; ivout < tx.vout.size() - 1; ivout++)  // except the last vout opret
            {
                if (tx.vout[ivout].scriptPubKey.IsPayToCryptoCondition())
                {
                    CScript opret;
                    CPubKey pk_in_opret;
                    SMarmaraCreditLoopOpret voutLoopData;
                    uint256 voutcreatetxid;

                    if (IsMarmaraLockedInLoopVout(tx, ivout, pk_in_opret, voutcreatetxid))
                        n++;
                }
            }

            if (n == 0)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "no locked-in-loop vouts in marmaratransfer prevtxid=" << prevtxid.GetHex() << std::endl);
                return -1;
            }
            return n;    
        }
        else 
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "incorrect funcid=" << (int)funcid << " in prevtxid=" << prevtxid.GetHex() << std::endl);    
    }
    else
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "could not get tx for prevtxid=" << prevtxid.GetHex() << std::endl);
    
    return -1;
}

static int32_t get_settlement_txid(uint256 &settletxid, uint256 issuetxid)
{
    int32_t vini, height;

    if (CCgetspenttxid(settletxid, vini, height, issuetxid, MARMARA_OPENCLOSE_VOUT) == 0)  // NOTE: CCgetspenttxid checks also mempool 
    {
        return 0;
    }
    return -1;
}

// load the create tx and adds data from its opret to loopData safely, with no overriding
static int32_t get_loop_creation_data(uint256 createtxid, struct SMarmaraCreditLoopOpret &loopData, uint8_t checkVersion)
{
    CTransaction tx;
    uint256 hashBlock;

    if (myGetTransaction(createtxid, tx, hashBlock) != 0 /*&& !hashBlock.IsNull()*/ && tx.vout.size() > 1)  // might be called from validation code, so non-locking version
    {
        uint8_t funcid;
        vscript_t vopret;

        // first check if this is really createtx to prevent override loopData with other tx type data:
        if (GetOpReturnData(tx.vout.back().scriptPubKey, vopret) && vopret.size() >= 2 && vopret[0] == EVAL_MARMARA && vopret[1] == MARMARA_CREATELOOP)  
        {
            if ((funcid = MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, checkVersion)) == MARMARA_CREATELOOP) {
                return(0); //0 is okay
            }
        }
    }
    return(-1);
}

static int32_t get_block_height(uint256 hashBlock)
{

    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end() && (*mi).second) {
        CBlockIndex* pindex = (*mi).second;
        if (chainActive.Contains(pindex)) {
            return pindex->GetHeight();
        }
    }

    return -1;
}

// consensus code:

// check total loop amount in tx and redistributed back amount (only for version 1.1):
static bool check_lcl_redistribution(const CTransaction &tx, uint256 prevtxid, int32_t startvin, std::set<int32_t> &usedccvouts, CAmount &loopAmount, int32_t &nPrevEndorsers, std::string &errorStr)
{
    uint256 batontxid0, createtxid;
    struct SMarmaraCreditLoopOpret creationLoopData;
    struct SMarmaraCreditLoopOpret currentLoopData;
    
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "checking prevtxid=" << prevtxid.GetHex() << std::endl);

    nPrevEndorsers = 0;
    // do not use MarmaraGetbatontxid here as the current tx is the last baton and we are not sure if it is already in the spent index, which is used by MarmaraGetbatontxid (so it might behave badly)
    if ((nPrevEndorsers = get_loop_endorsers_number(createtxid, prevtxid, MARMARA_OPRET_VERSION_DEFAULT)) < 0) {   // number of endorsers + issuer, without the current tx
        errorStr = "could not get credit loop endorsers number";
        return false;
    }
    if (get_loop_creation_data(createtxid, creationLoopData, MARMARA_OPRET_VERSION_DEFAULT) < 0)
    {
        errorStr = "could not get credit loop creation data";
        return false;
    }

    // get opret data
    if (tx.vout.size() == 0 || MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, currentLoopData, MARMARA_OPRET_VERSION_DEFAULT) == 0)
    {
        errorStr = "no opreturn found in the last vout of issue/transfer tx ";
        return false;
    }

    // check loop endorsers are funded correctly:
    CAmount lclAmount = 0L;
    std::list<CPubKey> endorserPks;
    for (int32_t ivout = 0; ivout < tx.vout.size() - 1; ivout ++)  // except the last vout opret
    {
        if (tx.vout[ivout].scriptPubKey.IsPayToCryptoCondition())
        {
            CScript opret;
            CPubKey pk_in_opret;
            SMarmaraCreditLoopOpret voutLoopData;
            uint256 voutcreatetxid;

            if (IsMarmaraLockedInLoopVout(tx, ivout, pk_in_opret, voutcreatetxid))
            {
                if (GetCCOpReturnData(tx.vout[ivout].scriptPubKey, opret) /*&& MarmaraDecodeLoopOpret(opret, voutLoopData) == MARMARA_LOCKED*/)
                {
                   /* this is checked in IsMarmaraLockedInLoopVout 
                   CPubKey createtxidPk = CCtxidaddr_tweak(NULL, createtxid); 
                   if (tx.vout[ivout] != MakeMarmaraCC1of2voutOpret(tx.vout[ivout].nValue, createtxidPk, opret))
                    {
                        errorStr = "MARMARA_LOCKED cc output incorrect: pubkey does not match";
                        return false;
                    }*/

                    if (voutcreatetxid != createtxid)
                    {
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "txid=" << tx.GetHash().GetHex() << " cc vout=" << ivout << " not from this loop, createtxid=" << createtxid.GetHex() << " vout createtxid=" << voutcreatetxid.GetHex() << std::endl);
                        errorStr = "cc vin=" + std::to_string(ivout) + " not from this loop";
                        return false;
                    }

                    MarmaraDecodeLoopOpret(opret, voutLoopData, MARMARA_OPRET_VERSION_DEFAULT);

                    // check each vout is 1/N lcl amount
                    CAmount  diff = tx.vout[ivout].nValue != creationLoopData.amount / (nPrevEndorsers + 1);
                    if (diff < -MARMARA_LOOP_TOLERANCE || diff > MARMARA_LOOP_TOLERANCE)
                    {
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "locked output amount incorrect: i=" << ivout << " nValue=" << tx.vout[ivout].nValue << " creationLoopData.amount=" << creationLoopData.amount << " nPrevEndorsers=" << nPrevEndorsers << " creationLoopData.amount / (nPrevEndorsers + 1)=" << (creationLoopData.amount / (nPrevEndorsers + 1)) << std::endl);
                        errorStr = "MARMARA_LOCKED cc output amount incorrect";
                        return false;
                    }

                    lclAmount += tx.vout[ivout].nValue;
                    endorserPks.push_back(voutLoopData.pk);

                    usedccvouts.insert(ivout);
                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "vout pubkey=" << HexStr(vuint8_t(voutLoopData.pk.begin(), voutLoopData.pk.end())) << " nValue=" << tx.vout[ivout].nValue << std::endl);
                }
                /* for issue tx no MARMARA_LOCKED vouts:
                else
                {
                    errorStr = "no MARMARA_LOCKED funcid found in cc opreturn";
                    return false;
                } */
            }
        }
    }

    // check loop amount:
    if (abs(creationLoopData.amount - lclAmount) > MARMARA_LOOP_TOLERANCE)  // should be llabs but can't change old consensus code
    {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "amounts do not match, creationLoopData.amount=" << creationLoopData.amount << " lclAmount=" << lclAmount << " creationLoopData.amount=" << creationLoopData.amount << std::endl);
        errorStr = "tx LCL amount invalid";
        return false;
    }

    // the latest endorser does not receive back to normal
    CPubKey latestpk = endorserPks.front();
    endorserPks.pop_front();

    if (nPrevEndorsers != endorserPks.size())   // now endorserPks is without the current endorser
    {
        errorStr = "incorrect number of endorsers pubkeys found in tx";
        return false;
    }

    if (nPrevEndorsers != 0)
    {
        // calc total redistributed amount to endorsers' normal outputs:
        CAmount redistributedAmount = 0L;
        for (const auto &v : tx.vout)
        {
            if (!v.scriptPubKey.IsPayToCryptoCondition()) // is normal
            {
                // check if a normal matches to any endorser pubkey
                for (const auto & pk : endorserPks) 
                {
                    if (v == CTxOut(v.nValue, CScript() << ParseHex(HexStr(pk)) << OP_CHECKSIG))
                    {
                        CAmount diff = v.nValue - creationLoopData.amount / nPrevEndorsers / (nPrevEndorsers+1);
                        if (diff < -MARMARA_LOOP_TOLERANCE || diff > MARMARA_LOOP_TOLERANCE)
                        {
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "normal output amount incorrect: v.nValue=" << v.nValue << " creationLoopData.amount=" << creationLoopData.amount << " nPrevEndorsers=" << nPrevEndorsers << " creationLoopData.amount / nPrevEndorsers / (nPrevEndorsers + 1)=" << (creationLoopData.amount / nPrevEndorsers / (nPrevEndorsers + 1)) << std::endl);
                            errorStr = "normal output amount incorrect";
                            return false;
                        }
                        redistributedAmount += v.nValue;
                        break; // found -> break, don't reuse;
                    }
                }
            }
        }
        // only one new endorser should remain without back payment to a normal output
        /*if (endorserPks.size() != 1)
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "invalid redistribution to normals: left endorserPks.size()=" << endorserPks.size() << std::endl);
            errorStr = "tx redistribution amount to normals invalid";
            return false;
        }*/

        // check that 'redistributed amount' == 1/N * 'loop amount' (where nPrevEndorsers == N-1)
        CAmount diff = lclAmount / (nPrevEndorsers + 1) - redistributedAmount;
        if (diff < -MARMARA_LOOP_TOLERANCE || diff > MARMARA_LOOP_TOLERANCE)
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "invalid total redistribution to normal outputs: lclAmount=" << lclAmount << " redistributedAmount=" << redistributedAmount << " nPrevEndorsers=" << nPrevEndorsers << " lclAmount / (nPrevEndorsers+1)=" << (lclAmount / (nPrevEndorsers + 1)) << std::endl);
            errorStr = "invalid total redistribution to normal outputs";
            return false;
        }
    }

    // enum spent locked-in-loop vins and collect pubkeys
    std::list<CPubKey> endorserPksPrev;
    for (int32_t i = startvin; i >= 0 && i < tx.vin.size(); i++)
    {
        if (IsCCInput(tx.vin[i].scriptSig))
        {
            if (cp->ismyvin(tx.vin[i].scriptSig))
            {
                CTransaction vintx;
                uint256 hashBlock;

                if (myGetTransaction(tx.vin[i].prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull()*/)
                {
                    CPubKey pk_in_opret;
                    uint256 voutcreatetxid;
                    if (IsMarmaraLockedInLoopVout(vintx, tx.vin[i].prevout.n, pk_in_opret, voutcreatetxid))   // if vin not added by AddMarmaraCCInputs
                    {
                        if (voutcreatetxid != createtxid) 
                        {
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "txid=" << tx.GetHash().GetHex() << " cc vin=" << i << " not from this loop, createtxid=" << createtxid.GetHex() << " vin createtxid=" << voutcreatetxid.GetHex() << std::endl);
                            errorStr = "cc vin=" + std::to_string(i) + " not from this loop";
                            return false;
                        }

                        endorserPksPrev.push_back(pk_in_opret);
                        LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "vintx pubkey=" << HexStr(vuint8_t(pk_in_opret.begin(), pk_in_opret.end())) << std::endl);
                    }
                    else
                    {
                        errorStr = "issue/transfer tx has unexpected non-lcl marmara cc vin";
                        return false;
                    }
                }
                else
                {
                    errorStr = "issue/transfer tx: can't get vintx for vin=" + std::to_string(i);
                    return false;
                }
            }
            else
            {
                errorStr = "issue/transfer tx cannot have non-marmara cc vins";
                return false;
            }
        }
    }

    // sort as pubkeys could be in any order in vintx / transfertx
    endorserPks.sort();
    endorserPksPrev.sort();
    if (endorserPks != endorserPksPrev)
    {
        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "LCL vintx pubkeys do not match vout pubkeys" << std::endl);
        for (const auto &pk : endorserPksPrev)
            LOGSTREAMFN("marmara", CCLOG_INFO, stream << "vintx pubkey=" << HexStr(vuint8_t(pk.begin(), pk.end())) << std::endl);
        for (const auto &pk : endorserPks)
            LOGSTREAMFN("marmara", CCLOG_INFO, stream << "vout pubkey=" << HexStr(vuint8_t(pk.begin(), pk.end())) << std::endl);
        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "popped vout last pubkey=" << HexStr(vuint8_t(latestpk.begin(), latestpk.end())) << std::endl);
        errorStr = "issue/transfer tx has incorrect loop pubkeys";
        return false;
    }

    loopAmount = creationLoopData.amount;
    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return true;
}

// check request or create tx 
static bool check_request_tx(uint256 requesttxid, CPubKey receiverpk, uint8_t issueFuncId, uint8_t checkVersion, std::string &errorStr)
{
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, NULL);

    // make sure less than maxlength (?)

    uint256 createtxid;
    struct SMarmaraCreditLoopOpret loopData;
    CTransaction requesttx;
    uint256 hashBlock;
    uint8_t funcid = 0;
    errorStr = "";

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "checking requesttxid=" << requesttxid.GetHex() << std::endl);

    if (requesttxid.IsNull())
        errorStr = "requesttxid can't be empty";
    else if (get_create_txid(createtxid, requesttxid, checkVersion) < 0)
        errorStr = "can't get createtxid from requesttxid (request tx could be in mempool, check version)";
    // check requested cheque params:
    else if (get_loop_creation_data(createtxid, loopData, checkVersion) < 0)
        errorStr = "cannot get loop creation data";
    else if (!myGetTransaction(requesttxid, requesttx, hashBlock))
        errorStr = "cannot get request transaction";
    // TODO: do we need here to check the request tx in mempool?
    // else if (hashBlock.IsNull())    /*is in mempool?*/
    //    errorStr = "request transaction still in mempool";
    else if (requesttx.vout.size() < 1 || (funcid = MarmaraDecodeLoopOpret(requesttx.vout.back().scriptPubKey, loopData, checkVersion)) == 0)
        errorStr = "cannot decode request tx opreturn data";
    else if (TotalPubkeyNormalInputs(requesttx, receiverpk) == 0)     // extract and check the receiver pubkey
        errorStr = "receiver pubkey does not match signer of request tx";
    else if (TotalPubkeyNormalInputs(requesttx, loopData.pk) > 0)     // extract and check the receiver pubkey
        errorStr = "sender pk signed request tx, cannot request credit from self";
// do not check this in validation: fails on syncing
//    else if (loopData.matures <= chainActive.LastTip()->GetHeight())
//        errorStr = "credit loop must mature in the future";

    else {
        if (issueFuncId == MARMARA_ISSUE && funcid != MARMARA_CREATELOOP)
            errorStr = "not a create tx";
        if (issueFuncId == MARMARA_TRANSFER && funcid != MARMARA_REQUEST)
            errorStr = "not a request tx";
    }
    
    if (!errorStr.empty()) 
        return false;
    else {
        LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for requesttx=" << requesttxid.GetHex() << std::endl);
        return true;
    }
}

// get cc balance to check if tx cc inputs and outputs are properly balanced
static CAmount get_cc_balance(const struct CCcontract_info *cp, const CTransaction &tx)
{
    CAmount ccInputs = 0L;
    CAmount ccOutputs = 0L;

    // get total for cc vintx
    for (auto const & vin : tx.vin)
    {
        if (IsCCInput(vin.scriptSig))
        {
            if (cp->ismyvin(vin.scriptSig))
            {
                CTransaction vintx;
                uint256 hashBlock;

                if (myGetTransaction(vin.prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull() should we allow checking mempool? This is safer, to allow to check mempool */)
                {
                    ccInputs += vintx.vout[vin.prevout.n].nValue;
                }
            }
        }
    }
    // get total for cc vouts
    for (auto const & vout : tx.vout)
    {
        if (vout.scriptPubKey.IsPayToCryptoCondition())
        {
            ccOutputs += vout.nValue;
        }
    }
    return ccInputs - ccOutputs;
}

// check issue or transfer tx for ver 1.1 (opret version == 1)
static bool check_issue_tx(const CTransaction &tx, std::string &errorStr)
{
    struct SMarmaraCreditLoopOpret loopData;
    std::set<int32_t> usedccvouts;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    if (tx.vout.size() == 0) {
        errorStr = "bad issue or transfer tx: no vouts";
        return false;
    }

    if (skipBadLoop(tx.GetHash()))
        return true;

    MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_DEFAULT);
    if (loopData.lastfuncid != MARMARA_ISSUE && loopData.lastfuncid != MARMARA_TRANSFER) {
        errorStr = "not an issue or transfer tx";
        return false;
    }

    CPubKey marmarapk = GetUnspendable(cp, NULL);
    CPubKey holderpk = loopData.pk;

    // check activated vins
    std::list<int32_t> nbatonvins;
    bool activatedHasBegun = false;
    int ivin = 0;
    for (; ivin < tx.vin.size(); ivin ++)
    {
        if (IsCCInput(tx.vin[ivin].scriptSig))
        {
            if (cp->ismyvin(tx.vin[ivin].scriptSig))
            {
                CTransaction vintx;
                uint256 hashBlock;

                if (myGetTransaction(tx.vin[ivin].prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull()*/)
                {
                    CPubKey pk_in_opret;
                    uint256 dummytxid;
                    if (IsMarmaraActivatedVout(vintx, tx.vin[ivin].prevout.n, pk_in_opret, dummytxid))   // if vin not added by AddMarmaraCCInputs
                    {
                        if (check_signing_pubkey(tx.vin[ivin].scriptSig) == marmarapk)
                        {
                            // disallow spending with marmara global privkey:
                            errorStr = "issue tx cannot spend activated coins using marmara global pubkey";
                            return false;
                        }
                        activatedHasBegun = true;
                    }
                    else
                    {
                        //    nbatonvins.push_back(i);                                            // this is probably baton or request tx
                        if (activatedHasBegun)  
                            break;          // activated vouts ended, break
                    }
                }
                else
                {
                    errorStr = "issue/transfer tx: can't get vintx for vin=" + std::to_string(ivin);
                    return false;
                }
            }
            else
            {
                errorStr = "issue/transfer tx cannot have non-marmara cc vins";
                return false;
            }
        }
    }

    // stop at find request tx, it is in the first cc input after added activated cc inputs:

    // if (nbatonvins.size() == 0)
    if (ivin >= tx.vin.size())
    {
        errorStr = "invalid issue/transfer tx: no request tx vin";
        return false;
    }
    //int32_t requesttx_i = nbatonvins.front();
    int32_t requesttx_i = ivin;
    //nbatonvins.pop_front();
    
    if (!check_request_tx(tx.vin[requesttx_i].prevout.hash, loopData.pk, loopData.lastfuncid, MARMARA_OPRET_VERSION_DEFAULT, errorStr)) {
        if (errorStr.empty())
            errorStr = "check_request_tx failed";
        return false;
    }

    // prev tx is either creation tx or baton tx (and not a request tx for MARMARA_TRANSFER)
    uint256 prevtxid;
    if (loopData.lastfuncid == MARMARA_ISSUE)
        prevtxid = tx.vin[requesttx_i].prevout.hash;

    if (loopData.lastfuncid == MARMARA_TRANSFER)
    {
        CTransaction vintx;
        uint256 hashBlock;

        //if (nbatonvins.size() == 0)
        if (++ivin >= tx.vin.size())
        {
            errorStr = "no baton vin in transfer tx";
            return false;
        }
        int32_t baton_i = ivin;
        //baton_i = nbatonvins.front();
        //nbatonvins.pop_front();

        // TODO: check that the baton tx is a cc tx:
        if (myGetTransaction(tx.vin[baton_i].prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull()*/)
        {
            if (!tx_has_my_cc_vin(cp, vintx)) {
                errorStr = "no marmara cc vins in baton tx for transfer tx";
                return false;
            }
        }
        prevtxid = tx.vin[baton_i].prevout.hash;
    }

    //if (nbatonvins.size() != 0)  // no other vins should present
    //{
    //    errorStr = "unknown cc vin(s) in issue/transfer tx";
    //    return false;
    //}
        

    //if (loopData.lastfuncid == MARMARA_TRANSFER)  // maybe for issue tx it could work too
    //{
    // check LCL fund redistribution and vouts in transfer tx
    ivin++;
    int32_t nPrevEndorsers = 0;
    CAmount loopAmount = 0;
    if (!check_lcl_redistribution(tx, prevtxid, ivin, usedccvouts, loopAmount, nPrevEndorsers, errorStr)) {
        if (errorStr.empty())
            errorStr = "check_lcl_redistribution failed";
        return false;
    }
    //}

    // check batons/markers
    if (tx.vout.size() <= MARMARA_BATON_VOUT || tx.vout[MARMARA_BATON_VOUT] != MakeCC1vout(EVAL_MARMARA, MARMARA_BATON_AMOUNT, holderpk)) {
        errorStr = "no marmara baton for issue/transfer tx";
        return false;
    }
    else
        usedccvouts.insert(MARMARA_BATON_VOUT);

    if (loopData.lastfuncid == MARMARA_ISSUE) 
    {
        if (tx.vout.size() <= MARMARA_OPENCLOSE_VOUT || tx.vout[MARMARA_OPENCLOSE_VOUT] != MakeCC1vout(EVAL_MARMARA, MARMARA_OPEN_MARKER_AMOUNT, marmarapk)) {
            errorStr = "no marmara open marker for issue tx";
            return false;
        }
        else
            usedccvouts.insert(MARMARA_OPENCLOSE_VOUT);
        if (tx.vout.size() <= MARMARA_LOOP_MARKER_VOUT || tx.vout[MARMARA_LOOP_MARKER_VOUT] != MakeCC1vout(EVAL_MARMARA, MARMARA_LOOP_MARKER_AMOUNT, marmarapk)) {
            errorStr = "no marmara open marker for issue tx";
            return false;
        }
        else
            usedccvouts.insert(MARMARA_LOOP_MARKER_VOUT);
    }

    // is there a change?
    for (int32_t i = 0; i < tx.vout.size() - 1; i++)  // except the last vout opret
    {
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition())
        {
            CPubKey pk_in_opret;
            uint256 dummytxid;

            if (IsMarmaraActivatedVout(tx, i, pk_in_opret, dummytxid))
            {
                usedccvouts.insert(i);
            }
        }
    }

    // check if all cc vout checked:
    std::set<int32_t> ccvouts;
    for (int32_t i = 0; i < tx.vout.size() - 1; i++)  // except the last vout opret
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition())
            ccvouts.insert(i);
    if (ccvouts != usedccvouts)
    {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "for issue/transfer tx=" << tx.GetHash().GetHex() << " unknown cc vouts, checked cc vouts=" << SET_TO_STRING(int32_t, usedccvouts) << " cc vouts=" << SET_TO_STRING(int32_t, ccvouts) << std::endl);
        errorStr = "unknown cc vout found";
        return false;
    }

    // check issue tx vouts...
    // ...checked in check_lcl_redistribution

    CAmount ccBatonsBalance, txbalance, balanceDiff;
    //CAmount txfee = 10000;
    if (loopData.lastfuncid == MARMARA_ISSUE)
        ccBatonsBalance = MARMARA_CREATETX_AMOUNT - (MARMARA_BATON_AMOUNT + MARMARA_LOOP_MARKER_AMOUNT + MARMARA_OPEN_MARKER_AMOUNT);
    else // MARMARA_TRANSFER
        ccBatonsBalance = (MARMARA_BATON_AMOUNT /*request baton*/ + MARMARA_BATON_AMOUNT /*prev baton*/ + loopAmount / (nPrevEndorsers + 1) /*loop/N*/) - (MARMARA_BATON_AMOUNT /*transfer baton*/);

    txbalance = get_cc_balance(cp, tx);
    balanceDiff = txbalance - ccBatonsBalance;
    if ((balanceDiff < -MARMARA_LOOP_TOLERANCE || balanceDiff > MARMARA_LOOP_TOLERANCE)) {
        errorStr = "invalid cc balance for issue/transfer tx";
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "invalid balance=" << txbalance << " needed=" << ccBatonsBalance << " for issue/transfer tx=" << tx.GetHash().GetHex() << std::endl);
        return false;
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return true;
}

// check issue or transfer tx for loop ver 1.2 (last opret ver==2)
static bool check_issue_tx_12(const CTransaction &tx, std::string &errorStr)
{
    struct SMarmaraCreditLoopOpret loopData, creationLoopData;
    std::set<int32_t> usedccvouts;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    if (tx.vout.size() == 0) {
        errorStr = "bad issue or transfer tx: no vouts";
        return false;
    }

    if (skipBadLoop(tx.GetHash()))
        return true;

    MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_LOOP12_VERSION);
    
    // route to the previous version validator:
    //if (loopData.version == 1)
    //    return check_issue_tx(tx, errorStr);

    if (loopData.version != 2)  {
        errorStr = "unsupported loop version";
        return false;
    }

    if (loopData.lastfuncid != MARMARA_ISSUE && loopData.lastfuncid != MARMARA_TRANSFER) {
        errorStr = "not an issue or transfer tx";
        return false;
    }

    if (get_loop_creation_data(loopData.createtxid, creationLoopData, MARMARA_OPRET_LOOP12_VERSION) < 0)
    {
        errorStr = "could not get credit loop creation data";
        return false;
    }

    CPubKey marmarapk = GetUnspendable(cp, NULL);
    CPubKey holderpk = loopData.pk;

    // check activated vins
    std::list<int32_t> nbatonvins;
    std::set<CPubKey> vinpks;
    CAmount CCActivatedInputs = 0LL;
    CAmount CCLockedInputs = 0LL;
    CAmount CCUnknownInputs = 0LL;
    bool bRequestTxChecked = false;
    bool bBatonTxChecked = false;

    for (int32_t ivin = 0; ivin < tx.vin.size(); ivin ++)
    {
        if (IsCCInput(tx.vin[ivin].scriptSig))
        {
            if (cp->ismyvin(tx.vin[ivin].scriptSig))
            {
                CTransaction vintx;
                uint256 hashBlock;

                if (myGetTransaction(tx.vin[ivin].prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull()*/)
                {
                    CPubKey pk_in_opret;
                    uint256 dummytxid, createtxid;
                    if (IsMarmaraActivatedVout(vintx, tx.vin[ivin].prevout.n, pk_in_opret, dummytxid))   // if vin not added by AddMarmaraCCInputs
                    {
                        if (check_signing_pubkey(tx.vin[ivin].scriptSig) == marmarapk)
                        {
                            // disallow spending with marmara global privkey:
                            errorStr = "issue tx cannot spend activated coins using marmara global pubkey";
                            return false;
                        }
                        CCActivatedInputs += vintx.vout[ tx.vin[ivin].prevout.n ].nValue;
                    }
                    else if (IsMarmaraLockedInLoopVout(vintx, tx.vin[ivin].prevout.n, pk_in_opret, createtxid))
                    {
                        if (createtxid != loopData.createtxid)  {
                            errorStr = "not this loop input";
                            return false;
                        }
                        CCLockedInputs += vintx.vout[ tx.vin[ivin].prevout.n ].nValue;
                        vinpks.insert(pk_in_opret);
                    }
                    else
                    {
                        if (!bRequestTxChecked)   {
                            if (!check_request_tx(tx.vin[ivin].prevout.hash, loopData.pk, loopData.lastfuncid, MARMARA_OPRET_LOOP12_VERSION, errorStr)) {
                                if (errorStr.empty())
                                    errorStr = "check_request_tx failed";
                                return false;
                            }
                            if (vintx.vout[tx.vin[ivin].prevout.n].nValue != MARMARA_CREATETX_AMOUNT && vintx.vout[tx.vin[ivin].prevout.n].nValue != MARMARA_BATON_AMOUNT)   {
                                errorStr = "invalid baton or request tx amount";
                                return false;
                            }
                            bRequestTxChecked = true;
                        }
                        else if (loopData.lastfuncid == MARMARA_TRANSFER && !bBatonTxChecked) {
                            // check baton tx
                            struct SMarmaraCreditLoopOpret vintxLoopData;
                            if (MarmaraDecodeLoopOpret(vintx.vout.back().scriptPubKey, vintxLoopData, MARMARA_OPRET_LOOP12_VERSION) == 0) {
                                errorStr = "could not parse prev tx loop data (check prev tx version)";
                                return false;
                            }
                            if (vintxLoopData.version != loopData.version)  {
                                errorStr = "invalid prev tx loop version";
                                return false;
                            }
                            if (vintxLoopData.createtxid != loopData.createtxid)  {
                                errorStr = "invalid prev tx loop createtxid";
                                return false;
                            }
                            if (!IsFuncidOneOf(vintxLoopData.lastfuncid, {MARMARA_ISSUE, MARMARA_TRANSFER}))  {
                                errorStr = "invalid prev tx loop funcid";
                                return false;
                            }
                            if (vintx.vout[tx.vin[ivin].prevout.n].nValue != MARMARA_BATON_AMOUNT)   {
                                errorStr = "invalid baton amount";
                                return false;
                            }
                            if (!tx_has_my_cc_vin(cp, vintx)) {
                                errorStr = "no marmara cc vins in previous baton tx for transfer tx";
                                return false;
                            }
                            bBatonTxChecked = true;
                        }
                        else
                            CCUnknownInputs += vintx.vout[tx.vin[ivin].prevout.n].nValue;
                    }
                }
                else
                {
                    errorStr = "issue/transfer tx: can't get vintx for vin=" + std::to_string(ivin);
                    return false;
                }
            }
            else
            {
                errorStr = "issue/transfer tx cannot have non-marmara cc vins";
                return false;
            }
        }
    }

    if (!bRequestTxChecked)     {
        errorStr = "request tx not found";
        return false;
    }
    if (loopData.lastfuncid == MARMARA_TRANSFER && !bBatonTxChecked)     {
        errorStr = "baton tx not found";
        return false;
    }

    // check outputs:
    CAmount lclAmount = 0LL;
    CAmount CCchange = 0LL;
    CAmount CCUnknownOutputs = 0LL;
    std::set<CPubKey> endorserPks;
    for (int32_t ivout = 0; ivout < tx.vout.size() - 1; ivout ++)  // except the last vout opret
    {
        if (tx.vout[ivout].scriptPubKey.IsPayToCryptoCondition())
        {
            CScript ccopret;
            CPubKey pk_in_opret;
            SMarmaraCreditLoopOpret voutLoopData;
            uint256 voutcreatetxid, dummytxid;

            // check markers and baton
            if (ivout == MARMARA_BATON_VOUT)  {
                if (tx.vout[ivout] != MakeCC1vout(EVAL_MARMARA, MARMARA_BATON_AMOUNT, holderpk))  {
                    errorStr = "invalid baton vout";
                    return false;
                }
                continue;
            }
            if (loopData.lastfuncid == MARMARA_ISSUE && ivout == MARMARA_LOOP_MARKER_VOUT)  {
                if (tx.vout[ivout] != MakeCC1vout(EVAL_MARMARA, MARMARA_LOOP_MARKER_AMOUNT, marmarapk)) {
                    errorStr = "invalid loop marker vout";
                    return false;
                }
                continue;
            }
            if (loopData.lastfuncid == MARMARA_ISSUE && ivout == MARMARA_OPENCLOSE_VOUT)  {
                if (tx.vout[ivout] != MakeCC1vout(EVAL_MARMARA, MARMARA_OPEN_MARKER_AMOUNT, marmarapk))    {
                    errorStr = "invalid loop open/close marker vout";
                    return false;
                }
                continue;
            }

            if (IsMarmaraLockedInLoopVout(tx, ivout, pk_in_opret, voutcreatetxid))
            {
                if (GetCCOpReturnData(tx.vout[ivout].scriptPubKey, ccopret) /*&& MarmaraDecodeLoopOpret(opret, voutLoopData) == MARMARA_LOCKED*/)
                {
                    if (voutcreatetxid != loopData.createtxid)
                    {
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "txid=" << tx.GetHash().GetHex() << " cc vout=" << ivout << " not from this loop, createtxid=" << loopData.createtxid.GetHex() << " vout createtxid=" << voutcreatetxid.GetHex() << std::endl);
                        errorStr = "cc vout=" + std::to_string(ivout) + " not from this loop";
                        return false;
                    }

                    MarmaraDecodeLoopOpret(ccopret, voutLoopData, MARMARA_OPRET_VERSION_DEFAULT);  // loop vdata still has ver 1

                    if (llabs(creationLoopData.amount/2 - tx.vout[ivout].nValue) > MARMARA_LOOP_TOLERANCE)    {
                        errorStr = "loop cc vout=" + std::to_string(ivout) + " amount out of tolerance";
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "loop vout out of tolerance creationLoopData.amount/2=" << creationLoopData.amount/2 << " loop vout amount=" << tx.vout[ivout].nValue << std::endl);
                        return false;
                    }

                    lclAmount += tx.vout[ivout].nValue;
                    endorserPks.insert(voutLoopData.pk);

                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "vout pubkey=" << HexStr(vuint8_t(voutLoopData.pk.begin(), voutLoopData.pk.end())) << " nValue=" << tx.vout[ivout].nValue << std::endl);
                }
            }
            else if (IsMarmaraActivatedVout(tx, ivout, pk_in_opret, dummytxid))
                CCchange += tx.vout[ivout].nValue;
            else
                CCUnknownOutputs += tx.vout[ivout].nValue;
        }
    }

    if (endorserPks != std::set<CPubKey>({ creationLoopData.pk, loopData.pk }))    {
        errorStr = "incorrect loop output pubkeys";
        return false;
    }

    if (CCUnknownInputs != 0)   {
        errorStr = "unknown cc inputs";
        return false;
    }
    if (CCUnknownOutputs != 0)   {
        errorStr = "unknown cc outputs";
        return false;
    }
    if (llabs(creationLoopData.amount - lclAmount) > MARMARA_LOOP_TOLERANCE)  {
        errorStr = "cc locked-in-loop vouts and loop amount out of tolerance";
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "out of tolerance creationLoopData.amount=" << creationLoopData.amount << " lclAmount=" << lclAmount << std::endl);
        return false;  
    }
    if (loopData.lastfuncid == MARMARA_ISSUE)   {
        if (CCLockedInputs != 0LL)  {
            errorStr = "locked-in-loop inputs not allowed for issue tx";
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "locked inputs not allowed for transfer tx CCLockedInputs=" << CCLockedInputs << std::endl);
            return false;  
        }
        if (llabs(CCActivatedInputs - (lclAmount + CCchange)) > MARMARA_LOOP_TOLERANCE)  {
            errorStr = "cc balance out of tolerance for issue tx";
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "issue tx out of tolerance CCActivatedInputs=" << CCActivatedInputs << " lclAmount=" << lclAmount << " CCchange=" << CCchange << std::endl);
            return false;  
        }
    }
    else {
        if (CCActivatedInputs != 0LL)  {
            errorStr = "activated inputs not allowed for transfer tx";
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "activated inputs not allowed for transfer tx CCActivatedInputs=" << CCActivatedInputs << std::endl);
            return false;  
        }
        if (CCchange != 0LL)  {
            errorStr = "activated outputs not allowed for transfer tx";
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "activated outputs not allowed for transfer tx CCchange=" << CCchange << std::endl);
            return false;  
        }
        if (llabs(CCLockedInputs - lclAmount) > MARMARA_LOOP_TOLERANCE)  {
            errorStr = "cc balance out of tolerance for transfer tx";
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "transfer tx out of tolerance CCLockedInputs=" << CCLockedInputs << " lclAmount=" << lclAmount << std::endl);
            return false;  
        }
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return true;
}


static bool check_settlement_tx(const CTransaction &settletx, std::string &errorStr)
{
    std::vector<uint256> creditloop;
    uint256 batontxid, createtxid;
    struct SMarmaraCreditLoopOpret creationLoopData;
    struct SMarmaraCreditLoopOpret settleLoopData;
    struct SMarmaraCreditLoopOpret batonLoopData;
    int32_t nPrevEndorsers = 0;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    // check settlement tx has vins and vouts
    if (settletx.vout.size() == 0) {
        errorStr = "bad settlement tx: no vouts";
        return false;
    }

    if (settletx.vin.size() == 0) {
        errorStr = "bad settlement tx: no vins";
        return false;
    }

    // fix bad settle tx:
    if (fixBadSettle(settletx.GetHash()))
        return true;

    // check settlement tx funcid
    MarmaraDecodeLoopOpret(settletx.vout.back().scriptPubKey, settleLoopData, MARMARA_OPRET_VERSION_ANY);   // allow to setlle either version
    if (settleLoopData.lastfuncid != MARMARA_SETTLE && settleLoopData.lastfuncid != MARMARA_SETTLE_PARTIAL) {
        errorStr = "not a settlement tx";
        return false;
    }

    // check settlement tx spends correct open-close baton
    if (settletx.vin[0].prevout.n != MARMARA_OPENCLOSE_VOUT) {
        errorStr = "incorrect settlement tx vin0";
        return false;
    }

    // check issue tx referred by settlement tx
    uint256 issuetxid = settletx.vin[0].prevout.hash;
    CTransaction issuetx;
    uint256 hashBlock;
    if (!myGetTransaction(issuetxid, issuetx, hashBlock) /*&& !hashBlock.IsNull()*/)
    {
        errorStr = "could not load issue tx";
        return false;
    }

    // call depending on loop version:
    if (settleLoopData.version == 1)    
    {
        if (!check_issue_tx(issuetx, errorStr)) {
            if (errorStr.empty())
                errorStr = "check_issue_tx failed";
            return false;
        }
    } 
    else if (settleLoopData.version == 2)    
    {
        if (!check_issue_tx_12(issuetx, errorStr)) {
            if (errorStr.empty())
                errorStr = "check_issue_tx_12 failed";
            return false;
        }
    } 
    else    
    {
        errorStr = "unsupported loop version for settlement";
        return false;
    }



    // fix bad issue tx spent:
    if (skipBadLoop(issuetxid))
        return true;

    // get baton txid and creditloop
    // NOTE: we can use MarmaraGetbatontxid here because the issuetx is not the last baton tx, 
    // the baton tx is always in the previous blocks so it is not the validated tx and there is no uncertainty about if the baton is or not in the indexes and coin cache
    if (MarmaraGetbatontxid(creditloop, batontxid, issuetxid) <= 0 || creditloop.empty()) {   // returns number of endorsers + issuer
        errorStr = "could not get credit loop or no endorsers";
        return false;
    }

    // get credit loop basic data (loop amount)
    createtxid = creditloop[0];
    if (get_loop_creation_data(createtxid, creationLoopData, settleLoopData.version) < 0)
    {
        errorStr = "could not get credit loop creation data";
        return false;
    }

    if (createtxid != settleLoopData.createtxid)
    {
        errorStr = "incorrect createtxid in settle tx";
        return false;
    }

    // check mature height:
    if (chainActive.LastTip()->GetHeight() < creationLoopData.matures)
    {
        errorStr = "credit loop does not mature yet";
        return false;
    }
    // get current baton tx
    CTransaction batontx;
    if (!myGetTransaction(batontxid, batontx, hashBlock) /*&& !hashBlock.IsNull()*/)
    {
        errorStr = "could not load baton tx";
        return false;
    }
    if (batontx.vout.size() == 0) {
        errorStr = "bad baton tx: no vouts";
        return false;
    }
    // get baton tx opret (we need holder pk from there)
    MarmaraDecodeLoopOpret(batontx.vout.back().scriptPubKey, batonLoopData, settleLoopData.version);
    if (batonLoopData.lastfuncid != MARMARA_ISSUE && batonLoopData.lastfuncid != MARMARA_TRANSFER) {
        errorStr = "baton tx not a issue or transfer tx";
        return false;
    }

    //find settled amount to the holder
    CAmount settledAmount = 0L;
    if (settletx.vout.size() > 0)
    {
        if (!settletx.vout[MARMARA_SETTLE_VOUT].scriptPubKey.IsPayToCryptoCondition())  // normals
        {
            if (settletx.vout[MARMARA_SETTLE_VOUT] == CTxOut(settletx.vout[MARMARA_SETTLE_VOUT].nValue, CScript() << ParseHex(HexStr(batonLoopData.pk)) << OP_CHECKSIG))
            {
                settledAmount = settletx.vout[MARMARA_SETTLE_VOUT].nValue;
            }
        }
    }

    for (const auto &v : settletx.vout)  // except the last vout opret
    {
        if (v.scriptPubKey.IsPayToCryptoCondition())  
        {
            // do not allow any cc vouts
            // NOTE: what about if change appears in settlement because someone has sent some coins to the loop address?
            // such coins should be either skipped by IsMarmaraLockedInLoopVout, because they dont have cc inputs
            // or such cc transactions will be rejected as invalid
            errorStr = "settlement tx cannot have unknown cc vouts";
            return false;
        }
    }

    // check cc balance:
    CAmount ccBalance = get_cc_balance(cp, settletx);
    if (ccBalance != settledAmount + MARMARA_OPEN_MARKER_AMOUNT) 
    {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "settlement tx incorrect cc balance=" << ccBalance << " settledAmount=" << settledAmount << " tx=" << HexStr(E_MARSHAL(ss << settletx)) << std::endl);
        errorStr = "settlement tx has incorrect cc balance";
        return false;
    }


    // check settled amount equal to loop amount
    CAmount diff = settledAmount - creationLoopData.amount;
    if (settleLoopData.lastfuncid == MARMARA_SETTLE && diff < 0)  
    {
        errorStr = "payment amount to holder incorrect for full settlement";
        return false;
    }
    // check settled amount less than loop amount for partial settlement
    if (settleLoopData.lastfuncid == MARMARA_SETTLE_PARTIAL && (diff >= 0 || settledAmount <= 0))
    {
        errorStr = "payment amount to holder incorrect for partial settlement";
        return false;
    }

    // check cc vins:
    for (int32_t i = 1; i < settletx.vin.size(); i++)
    {
        if (cp->ismyvin(settletx.vin[i].scriptSig))
        {
            CTransaction vintx;
            if (myGetTransaction(settletx.vin[i].prevout.hash, vintx, hashBlock) /*&& !hashBlock.IsNull()*/ /*allow mempool*/)
            {
                CPubKey pk_in_opret;
                uint256 vincreatetxid;
                if (IsMarmaraLockedInLoopVout(vintx, settletx.vin[i].prevout.n, pk_in_opret, vincreatetxid))
                {
                    if (vincreatetxid != createtxid)
                    {
                        errorStr = "in settlement tx found not this loop cc vin txid=" + settletx.vin[i].prevout.hash.GetHex() + " n=" + std::to_string(settletx.vin[i].prevout.n);
                        return false;
                    }
                }
                else
                {
                    errorStr = "in settlement tx found not a locked-in-loop vin txid=" + settletx.vin[i].prevout.hash.GetHex() + " n=" + std::to_string(settletx.vin[i].prevout.n);
                    return false;
                }
            }
            else
            {
                errorStr = "for settlement tx could not load vintx txid=" + settletx.vin[i].prevout.hash.GetHex();
                return false;
            }
        }
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "validation okay for tx=" << settletx.GetHash().GetHex() << std::endl);
    return true;
}

// tx could be either staketx or activated tx that is always spent to self
// check that the tx's spent and sent balances match
// check vout match pk in cc opret 
static bool check_stake_tx(bool isLocked, const CTransaction &tx, std::string &errorStr)
{
    std::map<std::string, CAmount> vout_amounts, vin_amounts;
    uint256 merkleroot;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, 0);

    // get all activated amounts in the tx and store for addresses
    if (tx.vout.size() != 2) {
        errorStr = "incorrect vout size in stake tx";
        return false;
    }
    
    if (!DecodeStakingOpRet(tx.vout[1].scriptPubKey, merkleroot))
    {
        errorStr = "no staking opreturn in stake tx";
        return false;
    }

    if (tx.vout[0].scriptPubKey.IsPayToCryptoCondition())
    {
        CPubKey opretpk;
        uint256 dummytxid, createtxid;
        bool isLocked = false;

        if (IsMarmaraActivatedVout(tx, 0, opretpk, dummytxid) || (isLocked = IsMarmaraLockedInLoopVout(tx, 0, opretpk, createtxid)))
        { 
            char coinaddr[KOMODO_ADDRESS_BUFSIZE];
            Getscriptaddress(coinaddr, tx.vout[0].scriptPubKey);
            // make map key to sort vout sums (actually only 1 vout)
            std::string scoinaddr = coinaddr + isLocked ? createtxid.ToString() : "";  // for LCL utxos add createtxid to the key to ensure that LCL utxo not sent to another loop
            vout_amounts[scoinaddr] += tx.vout[0].nValue;
        }
    }

    for (int32_t i = 0; i < tx.vin.size(); i++)
    {
        // allow several vins for future if stake tx aggregation will be implemented
        if (cp->ismyvin(tx.vin[i].scriptSig))
        {
            CTransaction vintx;
            uint256 hashBlock;

            if (myGetTransaction(tx.vin[i].prevout.hash, vintx, hashBlock))
            {
                CPubKey opretpk;
                uint256 dummytxid, createtxid;
                bool isLocked = false;

                if (IsMarmaraActivatedVout(tx, i, opretpk, dummytxid) || (isLocked = IsMarmaraLockedInLoopVout(tx, i, opretpk, createtxid)))
                {
                    int32_t n = tx.vin[i].prevout.n;
                    char coinaddr[KOMODO_ADDRESS_BUFSIZE];
                    Getscriptaddress(coinaddr, vintx.vout[n].scriptPubKey);
                    // make map key to sort vin sums
                    std::string scoinaddr = coinaddr + isLocked ? createtxid.ToString() : "";  // for LCL utxos add createtxid to the key to ensure that LCL utxo not sent to another loop
                    vin_amounts[scoinaddr] += vintx.vout[n].nValue;
                }
            }
        }
    }

    if (vin_amounts.size() > 0 && vin_amounts == vout_amounts)  // compare should be okay as maps are sorted    
    {
        LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
        return true;
    }
    else
    {
        errorStr = "spending activated tx is allowed only to self";
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "activated tx vin/vout unbalanced:" << std::endl);
        for (const auto &vinam : vin_amounts)
            LOGSTREAMFN("marmara", CCLOG_INFO, stream << "activated tx vin address=" << vinam.first << " amount=" << vinam.second << std::endl);
        for (const auto &voutam : vout_amounts)
            LOGSTREAMFN("marmara", CCLOG_INFO, stream << "activated tx vout address=" << voutam.first << " amount=" << voutam.second << std::endl);
        return false;
    }
}


// check global pk vout is spent (only markers could be here)
static bool check_global_spent_tx(const CTransaction &tx, const std::set<uint8_t> &funcids, std::string &error)
{
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, 0);

    for (int32_t i = 0; i < tx.vin.size(); i++)
    {
        if (cp->ismyvin(tx.vin[i].scriptSig))
        {
            CTransaction vintx;
            uint256 hashBlock;

            if (myGetTransaction(tx.vin[i].prevout.hash, vintx, hashBlock))
            {
                int32_t n = tx.vin[i].prevout.n;

                if (MakeCC1vout(EVAL_MARMARA, vintx.vout[n].nValue, Marmarapk) == vintx.vout[n]) {
                    // marker spending found
                    if (n == MARMARA_LOOP_MARKER_AMOUNT) {
                        error = "can't spend loop marker";
                        return false; // can't spend loop marker
                    }
                    if (n == MARMARA_OPENCLOSE_VOUT) {
                        // only settlement tx can spend marker:
                        if (funcids != std::set<uint8_t>{MARMARA_SETTLE} &&
                            funcids != std::set<uint8_t>{MARMARA_SETTLE_PARTIAL}) {
                            error = "only settlement tx can close loop";
                            return false; // can't spend loop marker
                        }
                    }
                    if (n == MARMARA_ACTIVATED_MARKER_AMOUNT) {
                        error = "can't spend activated address marker";
                        return false; // can't spend markers of activated addresses
                    }
                }
            }
        }
    }
    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return true;
}

CAmount get_txfee(const CTransaction &tx)
{
    CAmount inputs = 0LL;
    CAmount outputs = 0LL;

    for (auto const &vin : tx.vin) 
    {
        CTransaction tx;
        uint256 hashBlock;

        //CAmount input = CCgettxout(vin.prevout.hash, vin.prevout.n, 0, 0);
        if (!myGetTransaction(vin.prevout.hash, tx, hashBlock))   {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " cannot get prev utxo with txid=" << vin.prevout.hash.GetHex() << " n=" << vin.prevout.n << std::endl);
            return -1;
        }
        inputs += tx.vout[vin.prevout.n].nValue;
    }
    for (auto const &vout : tx.vout) 
        outputs += vout.nValue;

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return inputs - outputs;
}

static bool check_release_tx(const CTransaction &tx, std::string &errorStr)
{
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey marmarapk = GetUnspendable(cp, 0);
    std::set<CPubKey> inputpks, outputpks;

    CAmount ccInputs = 0LL;
    for (auto const &vin  : tx.vin)  
    {
        if (cp->ismyvin(vin.scriptSig)) 
        {
            CTransaction vintx;
            uint256 hashBlock;

            // check no global pk spending in releasing:
            if (check_signing_pubkey(vin.scriptSig) == marmarapk)  {
                errorStr = "can't spend with global pk";
                return false; 
            }
            if (myGetTransaction(vin.prevout.hash, vintx, hashBlock))
            {
                CPubKey opretpk;
                uint256 dummytxid;
                // only activated account are allowed to spend
                if (IsMarmaraActivatedVout(vintx, vin.prevout.n, opretpk, dummytxid))   {
                    ccInputs += vintx.vout[vin.prevout.n].nValue;
                    inputpks.insert(opretpk);
                }
                else {
                    errorStr = "can't spend non-activated account";
                    return false;
                }
            }
        }
    }

    if (inputpks.size() > 1)   {
        errorStr = "only one pk is allowed";
        return false;
    }

    CAmount normalOutputs = 0LL;
    CAmount ccOutputs = 0LL;
    for (int32_t i = 0; i < tx.vout.size(); i ++)    {
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition()) 
        {
            CPubKey opretpk;
            uint256 dummytxid;
            if (IsMarmaraActivatedVout(tx, i, opretpk, dummytxid))   {
                ccOutputs += tx.vout[i].nValue;
                outputpks.insert(opretpk);
            }
            else    {
                errorStr = "non-activated output not allowed";
                return false; 
            }
        }
        else
            normalOutputs += tx.vout[i].nValue;
    }

    // check change to self:
    if (outputpks.size() > 0 && inputpks != outputpks)     {
        errorStr = "cc change should go to self pk";
        return false;
    }

    // check released amount:
    if (ccInputs < ccOutputs)  {
        errorStr = "cc inputs less than cc outputs";
        return false; 
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " validation okay for tx=" << tx.GetHash().GetHex() << std::endl);
    return true;
}


//#define HAS_FUNCID(v, funcid) (std::find((v).begin(), (v).end(), funcid) != (v).end())

bool MarmaraValidate(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn)
{
    // activate h0 consensus
    if (get_next_height() < MARMARA_POS_IMPROVEMENTS_HEIGHT) {

        bool h0error = MarmaraValidate_h0(cp, eval, tx, nIn);
        if (!h0error)
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " h0 validation error: '" << eval->state.GetRejectReason() << "' for tx=" << HexStr(E_MARSHAL(ss << tx)) << std::endl);
        return h0error;
    }

    if (!ASSETCHAINS_MARMARA)
        return eval->Invalid("-ac_marmara must be set for marmara CC");

    if (tx.vout.size() < 1)
        return eval->Invalid("no vouts");

    CPubKey Marmarapk = GetUnspendable(cp, 0);
    std::string validationError;
    std::set<uint8_t> funcIds;
    int nDoS = 100; // DoS level by default

    for (int32_t i = 0; i < tx.vout.size(); i++)
    {

        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition())
        {
            CPubKey opretpk;
            CScript opret;
            CMarmaraActivatedOpretChecker activatedChecker;
            CMarmaraLockInLoopOpretChecker lockinloopChecker( CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT );  // loop vouts cc data have only ver==1

            // just get opreturn funcid
            if (get_either_opret(&activatedChecker, tx, i, opret, opretpk)) 
            {
                CPubKey pk;
                int32_t h, uh;

                uint8_t funcid = MarmaraDecodeCoinbaseOpret(opret, pk, h, uh);
                funcIds.insert(funcid);
            }
            else if (get_either_opret(&lockinloopChecker, tx, i, opret, opretpk))
            {
                struct SMarmaraCreditLoopOpret loopData;
                MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_ANY);
                funcIds.insert(loopData.lastfuncid);
            }
        }

        // release coin opret support:
        if (i == tx.vout.size() - 1)    
        {
            struct SMarmaraCreditLoopOpret loopData;
            if (MarmaraDecodeLoopOpret(tx.vout[i].scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY) != 0)  // decode either version, later it will be checked
                funcIds.insert(loopData.lastfuncid);
            else 
            {
                uint8_t version;
                uint8_t funcid = MarmaraDecodeReleaseOpret(tx.vout[i].scriptPubKey, version, 1);
                if (funcid != 0)
                    funcIds.insert(funcid);
            }
        }
    }

    CAmount txfee;
    const CAmount max_txfee = 2 * 10000;  //FinalizeCCtx adds change if inputs-outputs > 2*txfee, txfee by default = 10000
    txfee = get_txfee(tx);
    if ((txfee < 0 || txfee > max_txfee))
    {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " validation error: '" << "bad txfee=" << txfee << "' for tx=" << HexStr(E_MARSHAL(ss << tx)) << std::endl);
        return eval->Error("incorrect or too big txfee");
    }

    if (check_global_spent_tx(tx, funcIds, validationError))  //need to be accurate with markers
    {
        if (funcIds.empty())
            validationError = "invalid or no opreturns";
        else if (funcIds == std::set<uint8_t>{MARMARA_POOL}) // TODO: pool tx disabled for now
        {
            int32_t ht, unlockht;
            CPubKey pk;

            uint8_t funcid = MarmaraDecodeCoinbaseOpret(tx.vout.back().scriptPubKey, pk, ht, unlockht);

            for (int32_t i = 0; i < tx.vin.size(); i++)
            {
                if ((*cp->ismyvin)(tx.vin[i].scriptSig) != 0)
                {
                    CTransaction vinTx;
                    uint256 hashBlock;

                    if (eval->GetTxUnconfirmed(tx.vin[i].prevout.hash, vinTx, hashBlock) == 0) {
                        validationError = "pool tx cant find vinTx";
                        break;
                    }
                    else
                    {
                        int32_t vht, vunlockht;
                        CPubKey vpk;

                        if (vinTx.IsCoinBase() == 0) {
                            validationError = "marmara pool noncoinbase input";
                            break;
                        }
                        else if (vinTx.vout.size() != 2) {
                            validationError = "marmara pool coinbase doesnt have 2 vouts";
                            break;
                        }
                        uint8_t vfuncid = MarmaraDecodeCoinbaseOpret(vinTx.vout[1].scriptPubKey, vpk, vht, vunlockht);
                        if (vfuncid != MARMARA_COINBASE || vpk != pk || vunlockht != unlockht) {
                            validationError = "marmara pool tx mismatched opreturn";
                            break;
                        }
                    }
                }
            }
            validationError = "marmara pool not supported yet";
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_LOOP}) // locked in loop funds 
        {
            // TODO: check this, seems error() is better than invalid():
            validationError = "unexpected tx funcid MARMARA_LOOP";   // this tx should have no cc inputs
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_CREATELOOP}) // create credit loop
        {
            validationError = "unexpected tx funcid MARMARA_CREATELOOP";   // this tx should have no cc inputs
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_REQUEST}) // receive -> agree to receive MARMARA_ISSUE from pk, amount, currency, due ht
        {
            validationError = "unexpected tx funcid MARMARA_REQUEST";   // tx should have no cc inputs
        }
        // issue -> issue currency to pk with due mature height:
        else if (funcIds == std::set<uint8_t>{MARMARA_ISSUE} ||
            funcIds == std::set<uint8_t>{MARMARA_ISSUE, MARMARA_LOCKED} ||
            funcIds == std::set<uint8_t>{MARMARA_ACTIVATED, MARMARA_ISSUE, MARMARA_LOCKED})
        {
            if (MarmaraIs2020JuneUpdateActive(eval))    {
                if (check_issue_tx_12(tx, validationError))
                    return true;
            }
            else    {
                if (check_issue_tx(tx, validationError))
                    return true;
            }
        }
        // transfer -> given MARMARA_REQUEST transfer MARMARA_ISSUE or MARMARA_TRANSFER to the pk of MARMARA_REQUEST:
        else if (funcIds == std::set<uint8_t>{MARMARA_TRANSFER} ||
            funcIds == std::set<uint8_t>{MARMARA_TRANSFER, MARMARA_LOCKED} ||
            funcIds == std::set<uint8_t>{MARMARA_ACTIVATED, MARMARA_TRANSFER, MARMARA_LOCKED})  // MARMARA_ACTIVATED could be if redistributed back 
        {
            if (MarmaraIs2020JuneUpdateActive(eval))    {
                if (check_issue_tx_12(tx, validationError))
                    return true;
            }
            else    {
                if (check_issue_tx(tx, validationError))
                    return true;
            }
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_SETTLE}) // settlement -> automatically spend issuers locked funds, given MARMARA_ISSUE
        {
            if (check_settlement_tx(tx, validationError))
                return true;
            nDoS = 0;   // do not ban settlement tx
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_SETTLE_PARTIAL}) // insufficient settlement
        {
            if (check_settlement_tx(tx, validationError))
                return true;
            nDoS = 0;   // do not ban settlement tx
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_COINBASE} || funcIds == std::set<uint8_t>{MARMARA_COINBASE_3X }) // coinbase 
        {
            if (check_stake_tx(false, tx, validationError))
                return true;
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_LOCKED}) // pk in lock-in-loop
        {
            if (check_stake_tx(true, tx, validationError))
                return true;
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_ACTIVATED} || funcIds == std::set<uint8_t>{MARMARA_ACTIVATED_INITIAL}) // activated
        {
            if (check_stake_tx(false, tx, validationError))
                return true;
        }
        else if (funcIds == std::set<uint8_t>{MARMARA_RELEASE} || funcIds == std::set<uint8_t>{MARMARA_RELEASE, MARMARA_ACTIVATED}) // released to normal
        {
            if (MarmaraIs2020JuneUpdateActive(eval))
            {
                if (check_release_tx(tx, validationError))
                    return true;
            }
            else {
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "unlock activated coin is not active yet" << std::endl);
            }
        }
    }

    if (validationError.empty())
        validationError = "invalid funcid combination";
        
    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " validation error '" << validationError << "' for tx=" << HexStr(E_MARSHAL(ss << tx)) << std::endl);
    return eval->Invalid(validationError, nDoS);
}
// end of consensus code



// set marmara coinbase opret for even blocks
// this is also activated coins opret
/* CScript MarmaraCoinbaseOpret(uint8_t funcid, const CPubKey &pk, int32_t height)
{
    uint8_t *ptr;
    //fprintf(stderr,"height.%d pksize.%d\n",height,(int32_t)pk.size());
    if (height > 0 && (height & 1) == 0 && pk.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE)
        return(MarmaraEncodeCoinbaseOpret(funcid, pk, height));
    else
        return(CScript());
} */

// returns coinbase scriptPubKey with 1of2 addr where coins will go in createNewBlock in miner.cpp 
// also adds cc opret
CScript MarmaraCreateDefaultCoinbaseScriptPubKey(int32_t nHeight, CPubKey minerpk)
{
    //std::cerr << __func__ << " nHeight=" << nHeight << std::endl;
    if (nHeight > 0 && (nHeight & 1) == 0)
    {
        char coinaddr[KOMODO_ADDRESS_BUFSIZE];
        CScript opret = MarmaraEncodeCoinbaseOpret(MARMARA_COINBASE, minerpk, nHeight);
        CTxOut ccvout; 
       
        if (minerpk.size() != CPubKey::COMPRESSED_PUBLIC_KEY_SIZE)
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "bad minerpk=" << HexStr(minerpk) << std::endl);
            return CScript();
        }

        // set initial amount to zero, it will be overriden by miner's code
        ccvout = MakeMarmaraCC1of2voutOpret(0, minerpk, opret);  // add cc opret to coinbase
        //LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for activated rewards using pk=" << HexStr(minerpk) << " height=" << nHeight << " 1of2addr=" << coinaddr << std::endl);
        return(ccvout.scriptPubKey);
    }
    else
    {
        //LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "not even ht, returning normal scriptPubKey" << std::endl);
        return CScript() << ParseHex(HexStr(minerpk)) << OP_CHECKSIG;
    }
}

// creates coinbase transaction for PoS blocks, adds marmara opreturn to coinbase
CScript MarmaraCreatePoSCoinbaseScriptPubKey(int32_t nHeight, const CScript &defaultspk, const CTransaction &staketx)
{
    CScript spk = defaultspk;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, 0);

    if (nHeight > 0 && (nHeight & 1) == 0) // for even heights
    {
        if (staketx.vout.size() > 0)
        {
            char checkaddr[KOMODO_ADDRESS_BUFSIZE];
            CScript opret;
            CPubKey opretpk;
            int32_t height;
            int32_t unlockht;
            vuint8_t vmypk = Mypubkey();
            CPubKey mypk = pubkey2pk(vmypk);  // add current miner pubkey to opret

            // for stake tx check only cc opret, in last-vout opret there is pos data:
            CMarmaraActivatedOpretChecker activatedChecker;
            CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);

            if (get_either_opret(&activatedChecker, staketx, 0, opret, opretpk))  // if stake tx has activatd vout
            {
                CScript coinbaseOpret;
                uint8_t version;
                int32_t matureht;
                bool is3x = false;
                uint8_t funcid = MarmaraDecodeCoinbaseOpretExt(opret, version, opretpk, height, unlockht, matureht);

                if (IsFuncidOneOf(funcid, MARMARA_ACTIVATED_3X_FUNCIDS)) { // if stake tx has 3x funcid
                    uint256 spenttxid;
                    int32_t spentvout;
                    int32_t spentheight;

                    if (nHeight >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
                    {
                        // if loop is not settled set 3x coinbase otherwise set 1x coinbase:
                        if (version == 2 &&  nHeight <= matureht) //loop not matured yet
                        {
                            coinbaseOpret = MarmaraEncodeCoinbaseOpretExt(2, MARMARA_COINBASE_3X, opretpk, nHeight, matureht);  // marmara 3x oprets create new 3x coinbases, add pubkey
                            is3x = true;
                        }
                        else
                        {
                            coinbaseOpret = MarmaraEncodeCoinbaseOpretExt(1, MARMARA_COINBASE, opretpk, nHeight, 0);  // create 1x coinbase
                        }
                    }
                    else
                    {
                        // old code simply sets 3x coinbase, no staker pubkey:
                        coinbaseOpret = MarmaraEncodeCoinbaseOpret(MARMARA_COINBASE_3X, opretpk, nHeight);
                    }
                }
                /*else if (IsFuncidOneOf(funcid, { MARMARA_ACTIVATED_INITIAL })) {  // for initially activated coins coinbase goes to miner pk
                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for initial activated stake tx created default coinbase scriptPubKey for miner pk" << std::endl);
                    spk = defaultspk;
                    return spk;
                }*/
                else 
                {
                    if (nHeight >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
                    {
                        // add no mature height:
                        coinbaseOpret = MarmaraEncodeCoinbaseOpretExt(1, MARMARA_COINBASE, opretpk, nHeight, 0);
                    }
                    else
                    {
                        // no stake pk in the old code
                        coinbaseOpret = MarmaraEncodeCoinbaseOpret(MARMARA_COINBASE, opretpk, nHeight);
                    }
                }
                CTxOut vout = MakeMarmaraCC1of2voutOpret(0, opretpk, coinbaseOpret);

                Getscriptaddress(checkaddr, vout.scriptPubKey);
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for activated stake tx created activated " << (is3x ? "3x" : "1x") << " coinbase scriptPubKey with address=" << checkaddr << std::endl); 
                spk = vout.scriptPubKey;
            }
            else if (get_either_opret(&lockinloopChecker, staketx, 0, opret, opretpk))  //if stake tx has locked in loop vout
            {               
                SMarmaraCreditLoopOpret loopdata;
                MarmaraDecodeLoopOpret(opret, loopdata, MARMARA_OPRET_VERSION_ANY);  //get loop createtxid to monitor when the loop is settled to switch from 3x to 1x

                CScript coinbaseOpret;

                if (nHeight >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
                {
                    SMarmaraCreditLoopOpret loopcrdata;
                    // always set 3x coinbase
                    // add mature hight to coinbase to track if loop is settled
                    get_loop_creation_data(loopdata.createtxid, loopcrdata, MARMARA_OPRET_VERSION_ANY);  // allow both loop version 1.1 or 1.2
                    coinbaseOpret = MarmaraEncodeCoinbaseOpretExt(2, MARMARA_COINBASE_3X, opretpk, nHeight, loopcrdata.matures);
                }
                else
                {
                    // old opret with no loop createtxid
                    coinbaseOpret = MarmaraEncodeCoinbaseOpret(MARMARA_COINBASE_3X, opretpk, nHeight);
                }
                CTxOut vout = MakeMarmaraCC1of2voutOpret(0, opretpk, coinbaseOpret);

                Getscriptaddress(checkaddr, vout.scriptPubKey);
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for lcl stake tx created activated 3x coinbase scriptPubKey address=" << checkaddr << std::endl);  
                spk = vout.scriptPubKey;
            }
            else
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cannot create pos marmara coinbase scriptPubKey, could not decode stake tx cc opret:" << staketx.vout[0].scriptPubKey.ToString() << std::endl);
            }
        }
        else
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cannot create pos marmara coinbase scriptPubKey, bad staketx:" << " staketx.vout.size()=" << staketx.vout.size() << std::endl);
        }

    }
    // old else: use default coinbase for odd heights
    else
    {
        if (nHeight >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
        {
            if (staketx.vout.size() > 0)
            {
                char checkaddr[KOMODO_ADDRESS_BUFSIZE];
                CScript opret;
                CPubKey opretpk;

                CMarmaraActivatedOpretChecker activatedChecker;
                CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);

                if (get_either_opret(&activatedChecker, staketx, 0, opret, opretpk))
                {
                    CScript coinbaseOpret;

                    CTxOut vout = CTxOut(0, CScript() << ParseHex(HexStr(opretpk)) << OP_CHECKSIG);
                    Getscriptaddress(checkaddr, vout.scriptPubKey);
                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for activated stake tx created normal coinbase scriptPubKey with address=" << checkaddr << " height=" << nHeight << std::endl);
                    spk = vout.scriptPubKey;
                }
                else if (get_either_opret(&lockinloopChecker, staketx, 0, opret, opretpk))
                {
                    CTxOut vout = CTxOut(0, CScript() << ParseHex(HexStr(opretpk)) << OP_CHECKSIG);

                    Getscriptaddress(checkaddr, vout.scriptPubKey);
                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "for lcl stake tx created normal coinbase scriptPubKey address=" << checkaddr << " height=" << nHeight << std::endl);
                    spk = vout.scriptPubKey;
                }
                else
                {
                    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cannot create pos marmara coinbase scriptPubKey, could not decode stake tx cc opret:" << staketx.vout[0].scriptPubKey.ToString() << " height=" << nHeight << std::endl);
                }
            }
        }
    }

    return spk;
}

// get pubkey from cc vout or PayToPK script
// this func is actually to get pubkey from coinbase or staketx
vuint8_t  MarmaraGetPubkeyFromSpk(const CScript &spk)
{
    vuint8_t vretpk;

    if (spk.IsPayToCryptoCondition())
    {
        CPubKey opretpk;
        CScript opret;

        if (GetCCOpReturnData(spk, opret))
        {
            CMarmaraActivatedOpretChecker activatedChecker;
            CMarmaraLockInLoopOpretChecker lclChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);

            if (activatedChecker.CheckOpret(opret, opretpk))
                vretpk = vuint8_t(opretpk.begin(), opretpk.end());
            else if (lclChecker.CheckOpret(opret, opretpk))
                vretpk = vuint8_t(opretpk.begin(), opretpk.end());
        }
    }
    else
    {
        if (spk.IsPayToPublicKey())
        {
            typedef std::vector<unsigned char> valtype;
            std::vector<valtype> vSolutions;
            txnouttype whichType;

            if (Solver(spk, whichType, vSolutions)) {
                vretpk = vSolutions[0];
            }
        }
    }
    
    return vretpk;
}

/*
vuint8_t MarmaraGetStakerPubkeyFromCoinbaseOpret(const CScript &spk)
{
    vuint8_t vretpk;

    if (spk.IsPayToCryptoCondition())
    {
        CScript opret;

        if (GetCCOpReturnData(spk, opret))
        {
            int32_t height, unlockht, matureht;
            uint8_t version;
            CPubKey opretpk, stakerpk;
            uint256  loopcreatetxid;
            if (MarmaraDecodeCoinbaseOpretExt(opret, version, opretpk, height, unlockht, matureht) != 0)
            {
                if (stakerpk.IsValid())
                    vretpk = vuint8_t(stakerpk.begin(), stakerpk.end());
            }
        }
    }
    return vretpk;
}
*/

CPubKey MarmaraGetMyPubkey()
{
    vuint8_t vmypk = Mypubkey();
    CPubKey mypk = pubkey2pk(vmypk);

    if (mypk.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE)
        return mypk;
 
    const bool avoidException = true;
    if (EnsureWalletIsAvailable(avoidException))
    {
        CReserveKey reservekey(pwalletMain);
        reservekey.GetReservedKey(mypk);
    }
    return mypk;
}


// half of the blocks (with even heights) should be mined as activated (to some unlock height)
// validates opreturn for even blocks
int32_t MarmaraValidateCoinbase(int32_t height, const CTransaction &tx, std::string &errmsg)
{ 
    if ((height & 1) != 0) // odd block - no marmara opret
    {
        return(0);  // TODO: do we need to check here that really no marmara coinbase opret for odd blocks?
    }
    else //even block - check for cc vout & opret
    {
        int32_t ht, unlockht; 
        CTxOut ccvout;
        struct CCcontract_info *cp, C;
        cp = CCinit(&C, EVAL_MARMARA);
        CPubKey Marmarapk = GetUnspendable(cp, NULL);

        if (tx.vout.size() >= 1 && tx.vout.size() <= 2) // NOTE: both cc and last vout oprets are supported in coinbases
        {
            CScript opret; 
            CPubKey dummypk, opretpk;
            CMarmaraActivatedOpretChecker activatedChecker;

            //vuint8_t d(tx.vout[0].scriptPubKey.begin(), tx.vout[0].scriptPubKey.end());
            //std::cerr << __func__ << " vtx cc opret=" << HexStr(d) << " height=" << height << std::endl;
            if (!get_either_opret(&activatedChecker, tx, 0, opret, dummypk))
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "can't find coinbase opret (this might normally happen sometimes on multiproc computers)" << " coinbase=" << HexStr(E_MARSHAL(ss << tx)) << std::endl);  
                errmsg = "marmara cc bad coinbase opreturn (this might normally happen sometimes on multiproc computers)";
                return -1;
            }

            uint8_t funcid = MarmaraDecodeCoinbaseOpret(opret, opretpk, ht, unlockht);
            if (IsFuncidOneOf( funcid, { MARMARA_COINBASE, MARMARA_COINBASE_3X } ))
            {
                //if (ht == height && MarmaraUnlockht(height) == unlockht)
                //{
                std::vector< vscript_t > vParams;
                CScript ccvoutCoinbase;

                ccvout = MakeCC1of2vout(EVAL_MARMARA, 0, Marmarapk, opretpk);   // TODO: check again if pk matches the address
                tx.vout[0].scriptPubKey.IsPayToCryptoCondition(&ccvoutCoinbase, vParams);
                if (ccvout.scriptPubKey == ccvoutCoinbase)
                    return 0;  // coinbase ok 

                char addr0[KOMODO_ADDRESS_BUFSIZE], addr1[KOMODO_ADDRESS_BUFSIZE];
                Getscriptaddress(addr0, ccvout.scriptPubKey);
                Getscriptaddress(addr1, tx.vout[0].scriptPubKey);
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " ht=" << height << " mismatched CCvout scriptPubKey=" << addr0 << " vs tx.vout[0].scriptPubKey=" << addr1 << " opretpk.size=" << opretpk.size() << " opretpk=" << HexStr(opretpk) << std::endl);
                //}
                //else
                //    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " ht=" << height << " MarmaraUnlockht=" << MarmaraUnlockht(height) << " vs opret's ht=" << ht << " unlock=" << unlockht << std::endl);
            }
            else
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " ht=" << height << " incorrect coinbase opret" << " funcid=" << (int)funcid << std::endl);
        }
        else
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " ht=" << height << " incorrect vout size for marmara coinbase" << std::endl);

        errmsg = "marmara cc constrains even height blocks to pay 100%% to CC in vout0 with opreturn";
        return(-1);
    }
}


const int32_t MARMARA_STAKE_TX_OK = 1;
const int32_t MARMARA_STAKE_TX_BAD = 0;

// for even block check coinbase pk matches stake tx opret pk and coinbase address == staketx address
bool check_pos_coinbase_opret(const CTransaction &coinbase, const CPubKey &staketx_opretpk, int32_t height)
{
    // pos improvements rules for lcl stake tx:
    if (height >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
    {
        struct CCcontract_info *cp, C;
        cp = CCinit(&C, EVAL_MARMARA);
        CPubKey Marmarapk = GetUnspendable(cp, 0);

        // check coinbase
        if (coinbase.vout.size() != 1) {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "invalid coinbase vout size" << " height=" << height << std::endl);
            return MARMARA_STAKE_TX_BAD;
        }

        if ((height & 0x01) == 0 && !coinbase.vout[0].scriptPubKey.IsPayToCryptoCondition())
        {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "even block pos coinbase scriptpubkey not a cc" << " height=" << height << std::endl);
            return MARMARA_STAKE_TX_BAD;
        }

        // for even block coinbase should go to the same address that stake tx is:
        char coinbaseaddr[KOMODO_ADDRESS_BUFSIZE];
        char checkaddr[KOMODO_ADDRESS_BUFSIZE];

        Getscriptaddress(coinbaseaddr, coinbase.vout[0].scriptPubKey);

        if ((height & 0x01) == 1)
        {
            // for odd blocks coinbase should go to normal address from pk from lcl 
            Getscriptaddress(checkaddr, CScript() << ParseHex(HexStr(staketx_opretpk)) << OP_CHECKSIG);
        }
        else
        {
            // more complicated check for even blocks
            uint8_t version;
            int32_t h, uh, matureht;
            uint256 loopcreatetxid;
            CPubKey cb_opretpk;
            CScript cb_opret;

            MyGetCCopret(coinbase.vout[0].scriptPubKey, cb_opret);
            if (MarmaraDecodeCoinbaseOpretExt(cb_opret, version, cb_opretpk, h, uh, matureht) == 0)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cant decode coinbase opret" << " height=" << height << " coinbase=" << HexStr(E_MARSHAL(ss << coinbase)) << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }
            if (cb_opretpk != staketx_opretpk) // check pk in coinbase == pk in staketx
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "incorrect pk in " << " height=" << height << " coinbase=" << HexStr(E_MARSHAL(ss << coinbase)) << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }

            // for even blocks coinbase should go to stake tx cc address
            GetCCaddress1of2(cp, checkaddr, Marmarapk, staketx_opretpk);
        }

        // for even blocks check coinbase address = staketx address
        if (strcmp(coinbaseaddr, checkaddr) != 0) {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "pos block coinbase should go to the 1of2 pubkey of stake tx" << " coinbaseaddr=" << coinbaseaddr << " checkaddr=" << checkaddr << " height=" << height << std::endl);
            return MARMARA_STAKE_TX_BAD;
        }
    }
    return MARMARA_STAKE_TX_OK;
}

// check marmara stake tx
// stake tx should have one cc vout and optional opret (in this case it is the cc opret)
// stake tx points to staking utxo in vintx
// stake tx vout[0].scriptPubKey equals the referred staking utxo scriptPubKey 
// and opret equals to the opret in the last vout or to the ccopret in the referred staking tx
// also validates coinbase for PoS blocks (coinbase should be sent to the same address that staketx)
// see komodo_staked() where stake tx is created
int32_t MarmaraValidateStakeTx(const char *destaddr, const CScript &vintxOpret, const CTransaction &staketx, const CTransaction &coinbase, int32_t height)
// note: the opret is fetched in komodo_txtime from cc opret or the last vout. 
// And that opret was added to stake tx by MarmaraSignature()
{
    // activation for h0 consensus is done in komodo_IsPos!

    uint8_t funcid; 

    /* this check is moved to komodo_staked to allow non-mining nodes to validate blocks
    // we need mypubkey set for stake_hash to work
    vuint8_t vmypk = Mypubkey();
    if (vmypk.size() == 0 || vmypk[0] == '\0')
    {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "no '-pubkey' set, please restart with -pubkey set for staking" << std::endl);
        return MARMARA_STAKE_TX_BAD;
    } */

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream  << "staketxid=" << staketx.GetHash().ToString() << " numvins=" << staketx.vin.size() << " numvouts=" << staketx.vout.size() << " vout[0].nValue="  << staketx.vout[0].nValue << " inOpret.size=" << vintxOpret.size() << std::endl);

    //check stake tx:
    if (staketx.vout.size() == 2 && staketx.vout[0].scriptPubKey.IsPayToCryptoCondition())
    {
        CScript opret;
        struct CCcontract_info *cp, C;
        cp = CCinit(&C, EVAL_MARMARA);
        CPubKey Marmarapk = GetUnspendable(cp, 0);
        CPubKey opretpk;
        char pkInOpretAddr[KOMODO_ADDRESS_BUFSIZE];

        // for stake tx check only cc opret, in last-vout opret there is pos data:
        CMarmaraActivatedOpretChecker activatedChecker;          
        CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);

        if (get_either_opret(&activatedChecker, staketx, 0, opret, opretpk))
        {
            if (vintxOpret != opret)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "found activated opret not equal to vintx opret, opret=" << opret.ToString() << " vintx opret=" << vintxOpret.ToString() << " h=" << height << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }

            GetCCaddress1of2(cp, pkInOpretAddr, Marmarapk, opretpk);

            // check stake tx spent to the prev stake tx address:
            if (strcmp(destaddr, pkInOpretAddr) != 0)  // check stake tx is spent to self
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "found bad activated opret" << " destaddr=" << destaddr << " not equal to 1of2 addr for pk in opret=" << pkInOpretAddr << " h=" << height << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }
            else
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found correct activated opret" << " destaddr=" << destaddr << std::endl);

            if (check_pos_coinbase_opret(coinbase, opretpk, height) == MARMARA_STAKE_TX_BAD)
                return MARMARA_STAKE_TX_BAD;
            
            return MARMARA_STAKE_TX_OK;

        }
        else if (get_either_opret(&lockinloopChecker, staketx, 0, opret, opretpk))
        {
            if (vintxOpret != opret)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "found bad lock-in-loop opret not equal to vintx opret, opret=" << opret.ToString() << " vintx opret=" << vintxOpret.ToString() << " h=" << height << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }
            
            struct SMarmaraCreditLoopOpret loopData;
            MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_DEFAULT);  // loop vouts have only ver 1
            CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);
            GetCCaddress1of2(cp, pkInOpretAddr, Marmarapk, createtxidPk);

            // check stake tx spent to the prev stake tx address:
            if (strcmp(destaddr, pkInOpretAddr) != 0)
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "found bad locked-in-loop stake tx opret" << " destaddr=" << destaddr << " not equal to 1of2 addr for pk in opret=" << pkInOpretAddr << " h=" << height << std::endl);
                return MARMARA_STAKE_TX_BAD;
            }
            else
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found correct locked-in-loop stake tx opret" << " destaddr=" << destaddr << std::endl);
        
            if (check_pos_coinbase_opret(coinbase, opretpk, height) == MARMARA_STAKE_TX_BAD)
                return MARMARA_STAKE_TX_BAD;
            
            return MARMARA_STAKE_TX_OK;
        }
    }
    
    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "incorrect stake tx vout number or opreturn, stake tx=" << HexStr(E_MARSHAL(ss << staketx)) << std::endl);
    return MARMARA_STAKE_TX_BAD;
}

#define MAKE_ACTIVATED_WALLET_DATA(key, pk, addr, segid, amount) std::make_tuple(key, pk, addr, segid, amount)

#define ACTIVATED_WALLET_DATA_KEY(d) std::get<0>(d)
#define ACTIVATED_WALLET_DATA_PK(d) std::get<1>(d)
#define ACTIVATED_WALLET_DATA_ADDR(d) std::get<2>(d)
#define ACTIVATED_WALLET_DATA_SEGID(d) std::get<3>(d)
#define ACTIVATED_WALLET_DATA_AMOUNT(d) std::get<4>(d)

typedef std::tuple<CKey, CPubKey, std::string, uint32_t, CAmount> tACTIVATED_WALLET_DATA;
typedef std::vector<tACTIVATED_WALLET_DATA> vACTIVATED_WALLET_DATA;

// enum activated 1of2 addr in the wallet:
static void EnumWalletActivatedAddresses(CWallet *pwalletMain, vACTIVATED_WALLET_DATA &activated)
{
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey marmarapk = GetUnspendable(cp, 0);

    std::set<CKeyID> setKeyIds;
    pwalletMain->GetKeys(setKeyIds);
    for (const auto &keyid : setKeyIds)
    {
        //std::cerr << "key=" << keyid.ToString()  << std::endl;
        CPubKey pk;
        if (pwalletMain->GetPubKey(keyid, pk))
        {
            CKey key;
            pwalletMain->GetKey(keyid, key);

            CMutableTransaction mtx;
            std::vector<CPubKey> pubkeys;
            char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
            GetCCaddress1of2(cp, activated1of2addr, marmarapk, pk);
            CAmount amount = AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, 0, CC_MAXVINS);
            if (amount > 0)
            {
                uint32_t segid = komodo_segid32(activated1of2addr) & 0x3f;
                tACTIVATED_WALLET_DATA tuple = MAKE_ACTIVATED_WALLET_DATA(key, pk, std::string(activated1of2addr), segid, amount);
                activated.push_back(tuple);
            }
            memset(&key, '\0', sizeof(key));
        }
        else
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "can't get pubkey from the wallet for keyid=" << keyid.ToString() << std::endl);
    }
}


static void EnumAllActivatedAddresses(std::vector<std::string> &activatedAddresses)
{
    char markeraddr[KOMODO_ADDRESS_BUFSIZE];
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > markerOutputs;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, NULL);

    GetCCaddress(cp, markeraddr, Marmarapk);
    SetCCunspents(markerOutputs, markeraddr, true);
    std::set<std::string> ccaddrset;

    // get all pubkeys who have ever activated coins:
    LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking markeraddr=" << markeraddr << std::endl);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = markerOutputs.begin(); it != markerOutputs.end(); it++)
    {
        CTransaction activatetx;
        uint256 hashBlock;
        uint256 marker_txid = it->first.txhash;
        int32_t marker_nvout = (int32_t)it->first.index;
        CAmount marker_amount = it->second.satoshis;

        //LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking tx on markeraddr txid=" << marker_txid.GetHex() << " vout=" << marker_nvout << std::endl);
        if (marker_amount == MARMARA_ACTIVATED_MARKER_AMOUNT)
        {
            if (myGetTransaction(marker_txid, activatetx, hashBlock) /*&& !hashBlock.IsNull()*/)
            {
                for(int32_t i = 0; i < activatetx.vout.size(); i++)
                {
                    if (activatetx.vout[i].nValue >= COIN && activatetx.vout[i].scriptPubKey.IsPayToCryptoCondition())
                    {
                        CScript opret;
                        CPubKey opretpk;
                        CMarmaraActivatedOpretChecker activatedChecker;

                        if (get_either_opret(&activatedChecker, activatetx, i, opret, opretpk))
                        {
                            char ccaddr[KOMODO_ADDRESS_BUFSIZE];
                            Getscriptaddress(ccaddr, activatetx.vout[i].scriptPubKey);
                            ccaddrset.insert(ccaddr);
                        }
                    }
                }
            }
            else
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error getting activated tx=" << marker_txid.GetHex() << std::endl);
        }
    }


    // store found activated addresses:
    for (auto const &a : ccaddrset) {
        activatedAddresses.push_back(a);
    }
    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "found activated addresses=" << activatedAddresses.size() << std::endl);
}


// enumerates activated cc vouts in the wallet or on mypk if wallet is not available
// calls a callback allowing to do something with the utxos (add to staking utxo array)
// TODO: maybe better to use AddMarmaraCCInputs with a callback for unification...
template <class T>
static void EnumActivatedCoins(T func, bool onlyLocal)
{
    std::vector<std::string> activatedAddresses;
#ifdef ENABLE_WALLET
    if (onlyLocal)
    {
        if (pwalletMain)
        {
            const CKeyStore& keystore = *pwalletMain;
            LOCK2(cs_main, pwalletMain->cs_wallet);
            vACTIVATED_WALLET_DATA activated;
            EnumWalletActivatedAddresses(pwalletMain, activated);
            for (const auto &a : activated)
                activatedAddresses.push_back(ACTIVATED_WALLET_DATA_ADDR(a));
        }
        else
        {
            // should not be here as it can't be PoS without a wallet
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "wallet not available" << std::endl);
            return;
        }
    }
#endif

    if (!onlyLocal)
        EnumAllActivatedAddresses(activatedAddresses);

    for (const auto &addr : activatedAddresses)
    {
        // add activated coins:
        std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > activatedOutputs;
        SetCCunspents(activatedOutputs, (char*)addr.c_str(), true);

        // add my activated coins:
        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking activatedaddr=" << addr << std::endl);
        for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = activatedOutputs.begin(); it != activatedOutputs.end(); it++)
        {
            CTransaction tx; uint256 hashBlock;
            CBlockIndex *pindex;

            uint256 txid = it->first.txhash;
            int32_t nvout = (int32_t)it->first.index;
            CAmount nValue = it->second.satoshis;

            if (nValue < COIN)   // skip small values
                continue;

            LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "check tx on activatedaddr with txid=" << txid.GetHex() << " vout=" << nvout << std::endl);

            if (myGetTransaction(txid, tx, hashBlock) && (pindex = komodo_getblockindex(hashBlock)) != 0 && myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, nvout) == 0)
            {
                char utxoaddr[KOMODO_ADDRESS_BUFSIZE] = "";

                Getscriptaddress(utxoaddr, tx.vout[nvout].scriptPubKey);
                if (strcmp(addr.c_str(), utxoaddr) == 0)  // check if actual vout address matches the address in the index
                                                          // because a key from vSolution[1] could appear in the addressindex and it does not match the address.
                                                          // This is fixed in this marmara branch but this fix is for discussion
                {
                    CScript opret;
                    CPubKey opretpk;
                    CMarmaraActivatedOpretChecker activatedChecker;

                    if (get_either_opret(&activatedChecker, tx, nvout, opret, opretpk))
                    {
                        CPubKey pk;
                        int32_t height;
                        int32_t unlockht;
                        bool is3x = IsFuncidOneOf(MarmaraDecodeCoinbaseOpret(opret, pk, height, unlockht), MARMARA_ACTIVATED_3X_FUNCIDS);

                        // call callback function:
                        func(addr.c_str(), tx, nvout, pindex);
                        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "found my activated 1of2 addr txid=" << txid.GetHex() << " vout=" << nvout << "  " << (is3x ? "3x" : "1x") << std::endl);
                    }
                    else
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "skipped activated 1of2 addr txid=" << txid.GetHex() << " vout=" << nvout << " cant decode opret" << std::endl);
                }
                else
                    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "skipped activated 1of2 addr txid=" << txid.GetHex() << " vout=" << nvout << " utxo addr and index not matched" << std::endl);
            }
        }
    }
}

// enumerates pk's locked in loop cc vouts
// pk could be null then all LCL coins enumerated
// calls a callback allowing to do something with the utxos (add to staking utxo array)
// TODO: maybe better to use AddMarmaraCCInputs with a callback for unification...
template <class T>
static void EnumLockedInLoopOld(T func, const CPubKey &pk)
{
    // Start timing
    auto start = std::chrono::high_resolution_clock::now();

    // Counter for SetCCunspents calls
    int setCCunspentsCalls = 0;

    char markeraddr[KOMODO_ADDRESS_BUFSIZE];
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> markerOutputs;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, NULL);

    GetCCaddress(cp, markeraddr, Marmarapk);

    // Increment the counter before calling SetCCunspents
    setCCunspentsCalls++;
    SetCCunspents(markerOutputs, markeraddr, true);

    // Enumerate all createtxids
    LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking markeraddr=" << markeraddr << std::endl);
    for (const auto& markerOutput : markerOutputs)
    {
        CTransaction isssuancetx;
        uint256 hashBlock;
        uint256 marker_txid = markerOutput.first.txhash;
        int32_t marker_nvout = static_cast<int32_t>(markerOutput.first.index);
        CAmount marker_amount = markerOutput.second.satoshis;

        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking tx on markeraddr txid=" << marker_txid.GetHex() << " vout=" << marker_nvout << std::endl);
        if (marker_nvout == MARMARA_LOOP_MARKER_VOUT && marker_amount == MARMARA_LOOP_MARKER_AMOUNT)
        {
            if (myGetTransaction(marker_txid, isssuancetx, hashBlock))
            {
                if (!isssuancetx.IsCoinBase() && isssuancetx.vout.size() > 2 && isssuancetx.vout.back().nValue == 0)
                {
                    struct SMarmaraCreditLoopOpret loopData;
                    if (MarmaraDecodeLoopOpret(isssuancetx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY) == MARMARA_ISSUE)
                    {
                        char loopaddr[KOMODO_ADDRESS_BUFSIZE];
                        std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> loopOutputs;
                        CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);

                        // Enumerate unspents in the loop
                        GetCCaddress1of2(cp, loopaddr, Marmarapk, createtxidPk);

                        // Increment the counter before calling SetCCunspents
                        setCCunspentsCalls++;
                        SetCCunspents(loopOutputs, loopaddr, true);

                        // Enumerate all locked-in-loop addresses
                        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking on loopaddr=" << loopaddr << std::endl);
                        for (const auto& loopOutput : loopOutputs)
                        {
                            CTransaction looptx;
                            uint256 hashBlock;
                            CBlockIndex *pindex;
                            uint256 txid = loopOutput.first.txhash;
                            int32_t nvout = static_cast<int32_t>(loopOutput.first.index);

                            LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "checking tx on loopaddr txid=" << txid.GetHex() << " vout=" << nvout << std::endl);

                            if (myGetTransaction(txid, looptx, hashBlock) && (pindex = komodo_getblockindex(hashBlock)) != nullptr && !myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, nvout))
                            {
                                if (!looptx.IsCoinBase() && !looptx.vout.empty())
                                {
                                    char utxoaddr[KOMODO_ADDRESS_BUFSIZE] = "";
                                    Getscriptaddress(utxoaddr, looptx.vout[nvout].scriptPubKey);

                                    if (strcmp(loopaddr, utxoaddr) == 0)
                                    {
                                        CScript opret;
                                        CPubKey pk_in_opret;

                                        CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);
                                        if (get_either_opret(&lockinloopChecker, looptx, nvout, opret, pk_in_opret))
                                        {
                                            if (!pk.IsValid() || pk == pk_in_opret)
                                            {
                                                // Call the callback function
                                                func(loopaddr, looptx, nvout, pindex);
                                                LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "found my lock-in-loop 1of2 addr txid=" << txid.GetHex() << " vout=" << nvout << std::endl);
                                            }
                                            else
                                            {
                                                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "skipped lock-in-loop 1of2 addr txid=" << txid.GetHex()
                                                                                           << " vout=" << nvout << " does not match the pk" << std::endl);
                                            }
                                        }
                                        else
                                        {
                                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "skipped lock-in-loop 1of2 addr txid=" << txid.GetHex()
                                                                                      << " vout=" << nvout << " can't decode opret" << std::endl);
                                        }
                                    }
                                    else
                                    {
                                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "skipped lock-in-loop 1of2 addr txid=" << txid.GetHex()
                                                                                  << " vout=" << nvout << " utxo addr and address index not matched" << std::endl);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error getting issuance tx=" << marker_txid.GetHex() << std::endl);
            }
        }
    }

    // Stop timing
    auto end = std::chrono::high_resolution_clock::now();

    // Compute duration in milliseconds
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Output execution time and number of SetCCunspents calls to stderr
    std::cerr << "[o] EnumLockedInLoopOld execution time: " << duration << " ms" << std::endl;
    std::cerr << "[o] SetCCunspents calls: " << setCCunspentsCalls << std::endl;
}

template <class T>
static void EnumLockedInLoop(T func, const CPubKey &pk)
{
    // Start timing
    auto start = std::chrono::high_resolution_clock::now();

    // Counter for SetCCunspents calls
    int setCCunspentsCalls = 0;

    char markeraddr[KOMODO_ADDRESS_BUFSIZE];
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> markerOutputs;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey Marmarapk = GetUnspendable(cp, NULL);

    GetCCaddress(cp, markeraddr, Marmarapk);
    setCCunspentsCalls++;
    SetCCunspents(markerOutputs, markeraddr, true);

    // Collect all unique loopaddrs
    std::set<std::string> uniqueLoopAddrs;
    for (const auto& markerOutput : markerOutputs)
    {
        CTransaction issuanceTx;
        uint256 hashBlock;
        uint256 marker_txid = markerOutput.first.txhash;
        int32_t marker_nvout = (int32_t)markerOutput.first.index;
        CAmount marker_amount = markerOutput.second.satoshis;

        if (marker_nvout == MARMARA_LOOP_MARKER_VOUT && marker_amount == MARMARA_LOOP_MARKER_AMOUNT)
        {
            if (myGetTransaction(marker_txid, issuanceTx, hashBlock))
            {
                if (!issuanceTx.IsCoinBase() && issuanceTx.vout.size() > 2 && issuanceTx.vout.back().nValue == 0)
                {
                    struct SMarmaraCreditLoopOpret loopData;
                    if (MarmaraDecodeLoopOpret(issuanceTx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY) == MARMARA_ISSUE)
                    {
                        char loopaddr[KOMODO_ADDRESS_BUFSIZE];
                        CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);
                        GetCCaddress1of2(cp, loopaddr, Marmarapk, createtxidPk);
                        uniqueLoopAddrs.insert(loopaddr);
                    }
                }
            }
        }
    }

    // Batch retrieve unspent outputs for all loopaddrs
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> allLoopOutputs;
    for (const auto& loopaddr : uniqueLoopAddrs)
    {
        std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> loopOutputs;
        setCCunspentsCalls++;
        SetCCunspents(loopOutputs, const_cast<char*>(loopaddr.c_str()), true);
        allLoopOutputs.insert(allLoopOutputs.end(), loopOutputs.begin(), loopOutputs.end());
    }

    // Process all unspent outputs
    for (const auto& loopOutput : allLoopOutputs)
    {
        CTransaction loopTx;
        uint256 hashBlock;
        CBlockIndex *pindex;
        uint256 txid = loopOutput.first.txhash;
        int32_t nvout = (int32_t)loopOutput.first.index;

        if (myGetTransaction(txid, loopTx, hashBlock) && (pindex = komodo_getblockindex(hashBlock)) != nullptr && !myIsutxo_spentinmempool(ignoretxid, ignorevin, txid, nvout))
        {
            if (!loopTx.IsCoinBase() && loopTx.vout.size() > 0)
            {
                char utxoaddr[KOMODO_ADDRESS_BUFSIZE] = "";
                Getscriptaddress(utxoaddr, loopTx.vout[nvout].scriptPubKey);

                if (uniqueLoopAddrs.find(utxoaddr) != uniqueLoopAddrs.end())
                {
                    CScript opret;
                    CPubKey pk_in_opret;

                    CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);
                    if (get_either_opret(&lockinloopChecker, loopTx, nvout, opret, pk_in_opret))
                    {
                        if (!pk.IsValid() || pk == pk_in_opret)
                        {
                            func(utxoaddr, loopTx, nvout, pindex);
                        }
                    }
                }
            }
        }
    }

    // Stop timing
    auto end = std::chrono::high_resolution_clock::now();

    // Compute duration in milliseconds
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Output execution time and number of SetCCunspents calls to stderr
    std::cerr << "[n] EnumLockedInLoop execution time: " << duration << " ms" << std::endl;
    std::cerr << "[n] SetCCunspents calls: " << setCCunspentsCalls << std::endl;
}

// add marmara special UTXO from activated and lock-in-loop addresses for staking
// called from PoS code
void MarmaraGetStakingUtxos(std::vector<struct komodo_staking> &array, int32_t *numkp, int32_t *maxkp, uint8_t *hashbuf, int32_t height)
{
    const char *logFName = __func__;

    // old behavior is add all activated and lcl utxos
    bool useLocalUtxos = false;
    CPubKey usePubkey;  // set as empty

    if (height >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
    {
        if ((height & 0x01) != 0)
        {
            // for odd blocks use only my utxos:
            useLocalUtxos = true;
            usePubkey = pubkey2pk(Mypubkey());
        }
        else if (height > 0)
        {
            if (GetArg(MARMARA_STAKE_PROVIDER_ARG, 0) != 0)
            {
                // started as a stake provider - use all utxo to stake for community
                useLocalUtxos = false;
            }
            else
            {
                // not a stake box - usual pos node with local utxos
                useLocalUtxos = true;
                usePubkey = pubkey2pk(Mypubkey());
            }
        }
    }
    else
    {
        useLocalUtxos = true;
        usePubkey = pubkey2pk(Mypubkey());
    }

    size_t utxoCountOld = 0;
    EnumLockedInLoopOld(
        [&](const char *loopaddr, const CTransaction & tx, int32_t nvout, CBlockIndex *pindex)
        {
            komodo_addutxo(array, numkp, maxkp, (uint32_t)pindex->nTime, (uint64_t)tx.vout[nvout].nValue, tx.GetHash(), nvout, (char*)loopaddr, hashbuf, tx.vout[nvout].scriptPubKey);
            LOGSTREAM("marmara", CCLOG_DEBUG2, stream << logFName << " " << "added utxo for staking locked-in-loop 1of2addr txid=" << tx.GetHash().GetHex() << " vout=" << nvout << std::endl);
            ++utxoCountOld;
        },
        usePubkey
    );

    size_t utxoCountNew = 0;
    // add all lock-in-loops utxos:
    EnumLockedInLoop(
        [&](const char *loopaddr, const CTransaction & tx, int32_t nvout, CBlockIndex *pindex)
        {
            komodo_addutxo(array, numkp, maxkp, (uint32_t)pindex->nTime, (uint64_t)tx.vout[nvout].nValue, tx.GetHash(), nvout, (char*)loopaddr, hashbuf, tx.vout[nvout].scriptPubKey);
            LOGSTREAM("marmara", CCLOG_DEBUG2, stream << logFName << " " << "added utxo for staking locked-in-loop 1of2addr txid=" << tx.GetHash().GetHex() << " vout=" << nvout << std::endl);
            ++utxoCountNew;
        },
        usePubkey
    );

    std::cerr << "utxoCountOld = " << utxoCountOld << ", utxoCountNew = " << utxoCountNew << std::endl;

    // add all activated utxos:

    EnumActivatedCoins(
        [&](const char *activatedaddr, const CTransaction & tx, int32_t nvout, CBlockIndex *pindex) 
        {
            komodo_addutxo(array, numkp, maxkp, (uint32_t)pindex->nTime, (uint64_t)tx.vout[nvout].nValue, tx.GetHash(), nvout, (char*)activatedaddr, hashbuf, tx.vout[nvout].scriptPubKey);
            LOGSTREAM("marmara", CCLOG_DEBUG2, stream << logFName << " " << "added utxo for staking activated 1of2 addr txid=" << tx.GetHash().GetHex() << " vout=" << nvout << std::endl);
        }, 
        useLocalUtxos
    );

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "added " << *numkp << " utxos for staking" << " height=" << height << std::endl);
    //return array;
}

// returns stake preferences for activated and locked utxos
int32_t MarmaraGetStakeMultiplier(const CTransaction & staketx, int32_t nvout)
{
    CScript opret;
    CPubKey opretpk;
    CMarmaraActivatedOpretChecker activatedChecker;                
    CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT);        // for stake tx check only cc opret, in last-vout opret there is pos data

    if (nvout >= 0 && nvout < staketx.vout.size()) // check boundary
    {
        LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "check staketx txid=" << staketx.GetHash().GetHex() << std::endl);
        if (staketx.vout[nvout].scriptPubKey.IsPayToCryptoCondition())
        {
            if (get_either_opret(&lockinloopChecker, staketx, nvout, opret, opretpk) /*&& mypk == opretpk - not for validation */)   // check if opret is lock-in-loop vout 
            {
                LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "check locked-in-loop opret okay, pk=" << HexStr(opretpk) << std::endl);

                struct SMarmaraCreditLoopOpret loopData;
                if (MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_ANY) != 0)
                {
                    //LOGSTREAMFN("marmara", CCLOG_DEBUG3, stream << "decode LCL opret okay" << std::endl);

                    struct CCcontract_info *cp, C;
                    cp = CCinit(&C, EVAL_MARMARA);
                    CPubKey Marmarapk = GetUnspendable(cp, NULL);

                    // get LCL address
                    char lockInLoop1of2addr[KOMODO_ADDRESS_BUFSIZE];
                    CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);
                    GetCCaddress1of2(cp, lockInLoop1of2addr, Marmarapk, createtxidPk);

                    // get vout address
                    char ccvoutaddr[KOMODO_ADDRESS_BUFSIZE];
                    Getscriptaddress(ccvoutaddr, staketx.vout[nvout].scriptPubKey);
                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "ccvoutaddr=" << ccvoutaddr << " lockInLoop1of2addr=" << lockInLoop1of2addr << std::endl);

                    if (strcmp(lockInLoop1of2addr, ccvoutaddr) == 0)  // check vout address is lock-in-loop address
                    {
                        CTransaction createtx;
                        uint256 hashBlock;
                        int32_t mult = 3;
                        int32_t height = 0;

                        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "utxo picked for stake with x" << mult << " as locked-in-loop" << " txid=" << staketx.GetHash().GetHex() << " nvout=" << nvout << std::endl);
                        return mult; // multiplier for locked-in-loop
                    }
                }
            }
            else if (get_either_opret(&activatedChecker, staketx, nvout, opret, opretpk))   // check if this is activated vout 
            {
                if (staketx.vout[nvout].scriptPubKey.IsPayToCryptoCondition())
                {
                    struct CCcontract_info *cp, C;
                    cp = CCinit(&C, EVAL_MARMARA);
                    CPubKey Marmarapk = GetUnspendable(cp, NULL);

                    char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
                    char ccvoutaddr[KOMODO_ADDRESS_BUFSIZE];
                    GetCCaddress1of2(cp, activated1of2addr, Marmarapk, opretpk/* mypk*/);
                    Getscriptaddress(ccvoutaddr, staketx.vout[nvout].scriptPubKey);
                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "ccvoutaddr=" << ccvoutaddr << " activated1of2addr=" << activated1of2addr << std::endl);

                    if (strcmp(activated1of2addr, ccvoutaddr) == 0)   // check vout address is opretpk activated address
                    {
                        vscript_t vopret;
                        uint8_t funcid = 0;
                        int32_t mult = 1;
                        GetOpReturnData(opret, vopret);

                        if (vopret.size() >= 2)
                            funcid = vopret[1];

                        if (IsFuncidOneOf(funcid, { MARMARA_COINBASE_3X }))  // is 3x stake tx?
                        {   
                            int32_t height = get_next_height();

                            if (height >= MARMARA_POS_IMPROVEMENTS_HEIGHT)
                            {
                                uint8_t version = 0;
                                int32_t h, uh, matureht = 0;
                                CPubKey opretpk;

                                // check if loop not settled yet
                                if (MarmaraDecodeCoinbaseOpretExt(opret, version, opretpk, h, uh, matureht) != 0 && version == 2 && height < matureht)
                                {
                                    mult = 3;
                                }
                                else
                                {
                                    if (version == 2)
                                        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "using mult=1 as current height=" << height << " is more or equal to mature height=" << matureht << " stake txid=" << staketx.GetHash().GetHex() << " nvout=" << nvout << std::endl);
                                }

                            }
                            else
                            {
                                // for old code do not check if loop settled
                                mult = 3;
                            }
                        }

                        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "utxo picked for stake with x" << mult << " as activated" << " txid=" << staketx.GetHash().GetHex() << " nvout=" << nvout << std::endl);
                        return mult;  // 1x or 3x multiplier for activated
                    }
                }
            }
        }
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "utxo not recognized for marmara stake" << " txid=" << staketx.GetHash().GetHex() << " nvout=" << nvout << std::endl);
    return 1;  //default multiplier 1x
}


// make activated by locking the amount on the max block height
UniValue MarmaraLock(const CPubKey &remotepk, int64_t txfee, int64_t amount, const CPubKey &paramPk)
{
    CMutableTransaction tmpmtx, mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ);
    struct CCcontract_info *cp, C;
    CPubKey Marmarapk, mypk, destPk;
    //int32_t unlockht, /*refunlockht,*/ nvout, ht, numvouts;
    int64_t inputsum = 0, change = 0;
    std::string rawtx, errorstr;
    // char mynormaladdr[KOMODO_ADDRESS_BUFSIZE];
    char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
    uint256 txid, hashBlock;
    CTransaction tx;
    uint8_t funcid;

    if (txfee == 0)
        txfee = 10000;

    int32_t height = komodo_nextheight();
    // as opret creation function MarmaraCoinbaseOpret creates opret only for even blocks - adjust this base height to even value
    if ((height & 1) != 0)
         height++;

    cp = CCinit(&C, EVAL_MARMARA);
    Marmarapk = GetUnspendable(cp, 0);

    bool isRemote = IS_REMOTE(remotepk);
    if (isRemote)
        mypk = remotepk;
    else
        mypk = pubkey2pk(Mypubkey());

    if (paramPk.IsValid())
        destPk = paramPk;
    else
        destPk = mypk;      // lock to self

/*    Getscriptaddress(mynormaladdr, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG);
    if ((val = CCaddress_balance(mynormaladdr, 0)) < amount) // if not enough funds in the wallet
        val -= 2 * txfee + MARMARA_ACTIVATED_MARKER_AMOUNT;    // dont take all, should al least 1 txfee remained 
    else
        val = amount; */

    CAmount amountToAdd = amount + MARMARA_ACTIVATED_MARKER_AMOUNT;
    //if (val > txfee) 
    //{
    inputsum = AddNormalinputs(mtx, mypk, amountToAdd + txfee, MARMARA_VINS, isRemote);  //added '+txfee' because if 'inputsum' exactly was equal to 'val' we'd exit from insufficient funds 
        /* do not need this as threshold removed from Addnormalinputs
        if (inputsum < val + txfee) {
            // if added inputs are insufficient
            // try to add value and txfee separately: 
            mtx.vin.clear();
            inputsum = AddNormalinputs(mtx, mypk, val, CC_MAXVINS / 2, isRemote);
            inputsum += AddNormalinputs(mtx, mypk, txfee, MARMARA_VINS, isRemote);
        }*/
    //}
    //fprintf(stderr,"%s added normal inputs=%.8f required val+txfee=%.8f\n", logFuncName, (double)inputsum/COIN,(double)(val+txfee)/COIN);

    CScript opret = MarmaraEncodeCoinbaseOpret(MARMARA_ACTIVATED, destPk, height);
    // lock the amount on 1of2 address:
    mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(amount, destPk, opret)); //add coinbase opret
    mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, MARMARA_ACTIVATED_MARKER_AMOUNT, Marmarapk));

    /* not used old code: adding from funds locked for another height
    if (inputsum < amount + txfee)  // if not enough normal inputs for collateral
    {
        //refunlockht = MarmaraUnlockht(height);  // randomized 

        result.push_back(Pair("normalfunds", ValueFromAmount(inputsum)));
        result.push_back(Pair("height", static_cast<int64_t>(height)));
        //result.push_back(Pair("unlockht", refunlockht));

        // fund remainder to add:
        remains = (amount + txfee) - inputsum;

        std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
        GetCCaddress1of2(cp, activated1of2addr, Marmarapk, mypk);
        SetCCunspents(unspentOutputs, activated1of2addr, true);
        //threshold = remains / (MARMARA_VINS + 1);
        uint8_t mypriv[32];
        Myprivkey(mypriv);
        CCaddr1of2set(cp, Marmarapk, mypk, mypriv, activated1of2addr);
        memset(mypriv,0,sizeof(mypriv));
    }
    */

    if (inputsum >= amountToAdd + txfee)
    {
        if (inputsum > amountToAdd + txfee)
        {
            change = (inputsum - amountToAdd - txfee);
            mtx.vout.push_back(CTxOut(change, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG));
        }
        rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, CScript()/*opret moved to cc vout*/, false);
        if (rawtx.size() == 0)
        {
            errorstr = "couldnt finalize CCtx";
        }
        else
        {
            result.push_back(Pair("result", "success"));
            result.push_back(Pair("hex", rawtx));
            return(result);
        }
    }
    else
        errorstr = (char *)"insufficient funds";
    result.push_back(Pair("result", "error"));
    result.push_back(Pair("error", errorstr));
    return(result);
}

// add stake tx opret, finalize and sign stake tx on activated or lock-in-loop 1of2 addr
// (note: utxosig bufsize = 512 is checked)
int32_t MarmaraSignature(uint8_t *utxosig, CMutableTransaction &mstaketx, int32_t height)
{
    uint256 txid, hashBlock; 
    CTransaction vintx; 
    int64_t txfee = 10000;

    // compatibility rules:

    // for marmara testers chain 
    /*bool lastVoutOpretDiscontinued = true;
    if (strcmp(ASSETCHAINS_SYMBOL, "MCL0") == 0)
    {
        CBlockIndex *tipindex = chainActive.Tip();
        if (tipindex)
        {
            if (tipindex->GetHeight() + 1 < 2000)
            {
                lastVoutOpretDiscontinued = false;
            }
        }
    }*/
    // end of compatibility rules

    if (myGetTransaction(mstaketx.vin[0].prevout.hash, vintx, hashBlock) && vintx.vout.size() > 0 /*was >1, but if ccopret could be only 1 vout*/ && mstaketx.vin[0].prevout.n < vintx.vout.size())
    {
        CScript finalOpret, vintxOpret;
        struct CCcontract_info *cp, C;
        cp = CCinit(&C, EVAL_MARMARA);
        uint8_t marmarapriv[32];
        CPubKey Marmarapk = GetUnspendable(cp, marmarapriv);

        CPubKey mypk = pubkey2pk(Mypubkey());  // no spending from mypk or any change to it is supposed, it is used just as FinalizeCCTx requires such param
        CPubKey opretpk;
        CMarmaraActivatedOpretChecker activatedChecker;
        CMarmaraLockInLoopOpretChecker lockinloopChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_DEFAULT); // ver==1 for cc opret

        if (get_either_opret(&activatedChecker, vintx, mstaketx.vin[0].prevout.n, vintxOpret, opretpk))  // note: opret should be ONLY in vintx ccvout
        {
            // sign activated staked utxo

            // decode utxo 1of2 address
            char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
            //uint8_t activatedpriv[32];
            

            //CKeyID keyid = opretpk.GetID();
            //CKey privkey;

            /*if ((height & 0x01) == 1)
            {
                //if (!pwalletMain || !pwalletMain->GetKey(keyid, privkey))
                //{
                //    LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "can't find user privkey or wallet not available" << std::endl);
                //    return 0;
                //}

                // use my privkey for odd blocks:
                Myprivkey(activatedpriv);
            }
            else
            {
                memcpy(activatedpriv, marmarapriv, sizeof(activatedpriv));
            }*/


            // if vintx has the last-vout opret then move it to cc-vout opret
            // check if cc vout opret exists in mtx
            /*CScript opret;
            bool hasccopret = false;
            if (GetCCOpReturnData(mstaketx.vout[0].scriptPubKey, opret))
            {
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "compatibility code: in mtx found ccopret=" << opret.ToString() << std::endl);
                if (activatedChecker.CheckOpret(opret, opretpk))
                {
                    hasccopret = true;
                }
            }*/

            Getscriptaddress(activated1of2addr, mstaketx.vout[0].scriptPubKey);

            //LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found activated opret in staking vintx" << std::endl);

            CC *probeCond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, opretpk);
            // use the global pk (instead of privkey for user's pubkey from the wallet):
            CCAddVintxCond(cp, probeCond, marmarapriv/*privkey.begin()*/);    //add probe condition to sign vintx 1of2 utxo
            cc_free(probeCond);

            //if (lastVoutOpretDiscontinued)
            finalOpret = CScript();  //empty for activated
            //else
            //    finalOpret = vintxOpret; // last-vout opret continues to be used until some height

            //memset(activatedpriv, '\0', sizeof(activatedpriv));  //wipe privkey

        }
        else if (get_either_opret(&lockinloopChecker, vintx, mstaketx.vin[0].prevout.n, vintxOpret, opretpk))   // note: opret could be in vintx ccvout
        {
            // sign lock-in-loop utxo

            struct SMarmaraCreditLoopOpret loopData;
            MarmaraDecodeLoopOpret(vintxOpret, loopData, MARMARA_OPRET_VERSION_DEFAULT);  // stake tx cc data has only ver 1

            CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);

            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found locked-in-loop opret in staking vintx" << std::endl);

            CC *probeCond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, createtxidPk);
            CCAddVintxCond(cp, probeCond, marmarapriv); //add probe condition to sign vintx 1of2 utxo
            cc_free(probeCond);

            //if (lastVoutOpretDiscontinued)
            finalOpret = CScript();  // empty last vout opret
            //else
            //    finalOpret = vintxOpret; // last-vout opret continues to be used until some height
        }

        // note: opreturn for stake tx is taken from the staking utxo (ccvout or back):
        std::string rawtx = FinalizeCCTx(0, cp, mstaketx, mypk, txfee, finalOpret, false);  // opret for LCL or empty for activated
        if (rawtx.size() > 0)
        {
            int32_t siglen = mstaketx.vin[0].scriptSig.size();
            uint8_t *scriptptr = &mstaketx.vin[0].scriptSig[0];

            if (siglen > 512) {   // check sig buffer limit
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "scriptSig length is more than utxosig bufsize, truncated! siglen=" << siglen << std::endl);
                siglen = 512;
            }

            std::ostringstream debstream;
            for (int32_t i = 0; i < siglen; i++)   {
                utxosig[i] = scriptptr[i];
                debstream << std::hex << (int)scriptptr[i];
            }
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "scriptSig=" << debstream.str() << " signed rawtx=" << rawtx << " siglen=" << siglen << std::endl);
            return(siglen);
        }
        else
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cannot sign marmara staked tx, bad mtx=" << HexStr(E_MARSHAL(ss << mstaketx)) << " opretpk=" << HexStr(opretpk) << std::endl);
    }
    else 
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cannot get vintx for staked tx" << std::endl);
    return(0);
}

// jl777: decide on what unlockht settlement change should have -> from utxo making change

UniValue MarmaraSettlement(int64_t txfee, uint256 refbatontxid, CTransaction &settlementTx)
{
    UniValue result(UniValue::VOBJ);
    std::vector<uint256> creditloop;
    uint256 batontxid;
    int32_t numerrs = 0, numDebtors;
    std::string rawtx;
    char loop1of2addr[KOMODO_ADDRESS_BUFSIZE], myCCaddr[KOMODO_ADDRESS_BUFSIZE], destaddr[KOMODO_ADDRESS_BUFSIZE], batonCCaddr[KOMODO_ADDRESS_BUFSIZE];
    struct CCcontract_info *cp, C;

    if (txfee == 0)
        txfee = 10000;

    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey minerpk = pubkey2pk(Mypubkey());
    uint8_t marmarapriv[32];
    CPubKey Marmarapk = GetUnspendable(cp, marmarapriv);

    int64_t change = 0;
    //int32_t height = chainActive.LastTip()->GetHeight();
    LOCK(cs_main);
    if ((numDebtors = MarmaraGetbatontxid(creditloop, batontxid, refbatontxid)) > 0)
    {
        CTransaction batontx;
        uint256 hashBlock;
        struct SMarmaraCreditLoopOpret loopData;

        if( get_loop_creation_data(creditloop[0], loopData, MARMARA_OPRET_VERSION_ANY) == 0 )
        {
            if (myGetTransaction(batontxid, batontx, hashBlock) && !hashBlock.IsNull() && batontx.vout.size() > 1)
            {
                uint8_t funcid;

                if ((funcid = MarmaraDecodeLoopOpret(batontx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY)) != 0) // update loopData with the baton opret
                {
                    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());

                    if (loopData.createtxid != creditloop[0])
                    {
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "invalid opret createtxid, should be set to creditloop[0]")); //TODO: note change
                        return(result);
                    }
                    else if (chainActive.LastTip()->GetHeight() < loopData.matures)
                    {
                        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "loop doesnt mature for another " << loopData.matures - chainActive.LastTip()->GetHeight() << " blocks" << std::endl);
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "cant settle immature creditloop"));
                        return(result);
                    }
                    /*else if ((loopData.matures & 1) == 0)
                    {
                        // discontinued:
                        //result.push_back(Pair("result", "error"));
                        //result.push_back(Pair("error", "cant automatic settle even maturity heights"));
                        //return(result);
                    }*/
                    else if (numDebtors < 1)
                    {
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "creditloop too short"));
                        return(result);
                    }
                    GetCCaddress(cp, myCCaddr, Mypubkey());
                    Getscriptaddress(batonCCaddr, batontx.vout[0].scriptPubKey);

                    // allow any miner to settle, do not check mypk:
                    //if (strcmp(myCCaddr, batonCCaddr) == 0) // if mypk user owns the baton
                    //{
                    std::vector<CPubKey> pubkeys;
                    uint256 issuetxid;

                    // note: can't spend the baton any more as settlement could be done by any miner
                    // spend the marker on marmara global pk
                    if (numDebtors > 1)
                        issuetxid = creditloop[1];
                    else
                        issuetxid = batontxid;

                    uint256 dummytxid;
                    int32_t dummyvin;
                    if (myIsutxo_spentinmempool(dummytxid, dummyvin, issuetxid, MARMARA_OPENCLOSE_VOUT))
                    {
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "loop already settled"));
                        return(result);
                    }

                    mtx.vin.push_back(CTxIn(issuetxid, MARMARA_OPENCLOSE_VOUT, CScript())); // spend vout2 marker - close the loop

                    // add tx fee from mypubkey
                    if (AddNormalinputs2(mtx, txfee, MARMARA_VINS) < txfee) {  // TODO: in the previous code txfee was taken from 1of2 address
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "cant add normal inputs for txfee"));
                        return(result);
                    }

                    char lockInLoop1of2addr[KOMODO_ADDRESS_BUFSIZE];
                    CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);
                    GetCCaddress1of2(cp, lockInLoop1of2addr, Marmarapk, createtxidPk);  // 1of2 lock-in-loop address

                    CC *lockInLoop1of2cond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, createtxidPk);
                    CCAddVintxCond(cp, lockInLoop1of2cond, marmarapriv); //add probe condition to spend from the lock-in-loop address
                    cc_free(lockInLoop1of2cond);

                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "calling AddMarmaraCCInputs for lock-in-loop addr=" << lockInLoop1of2addr << " adding amount=" << loopData.amount << std::endl);
                    CAmount lclAmount = AddMarmaraCCInputs(IsMarmaraLockedInLoopVout, mtx, pubkeys, lockInLoop1of2addr, loopData.amount, MARMARA_VINS);
                    if (lclAmount >= loopData.amount)
                    {
                        // not sure where to send the change, let's send all to the holder
                        // change = (lclAmount - loopData.amount);
                        mtx.vout.push_back(CTxOut(/*loopData.amount*/ lclAmount, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG));   // locked-in-loop money is released to mypk doing the settlement
                        /*if (change > txfee) {
                            CScript opret;
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error: change not null=" << change << ", sent back to lock-in-loop addr=" << lockInLoop1of2addr << std::endl);
                            mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(change, createtxidPk, opret));  // NOTE: change will be rejected by the current validation code
                        }*/
                        rawtx = FinalizeCCTx(0, cp, mtx, minerpk, txfee, MarmaraEncodeLoopSettlementOpret(loopData.version, true, loopData.createtxid, loopData.pk, 0), false);
                        if (rawtx.empty()) {
                            result.push_back(Pair("result", "error"));
                            result.push_back(Pair("error", "could not finalize CC Tx"));
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "FinalizeCCTx error bad settlement mtx=" << HexStr(E_MARSHAL(ss << mtx)) << std::endl);
                        }
                        else {
                            result.push_back(Pair("result", "success"));
                            result.push_back(Pair("hex", rawtx));
                            settlementTx = mtx;
                        }
                        return(result);
                    }
                    else if (lclAmount > 0)
                    {
                        CAmount remaining = loopData.amount - lclAmount;

                        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "trying to partial settle loop, initial amount=" << loopData.amount << " actual amount=" << lclAmount << std::endl);
                        
                        // TODO: seems this was supposed that txfee should been taken from 1of2 address?
                        //if (refamount - remaining > 3 * txfee)
                        //    mtx.vout.push_back(CTxOut(refamount - remaining - 2 * txfee, CScript() << ParseHex(HexStr(mypk)) << OP_CHECKSIG));
                        mtx.vout.push_back(CTxOut(lclAmount /*- txfee*/, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG)); // MARMARA_SETTLE_VOUT is 0

                        //mtx.vout.push_back(CTxOut(txfee, CScript() << ParseHex(HexStr(CCtxidaddr_tweak(NULL, loopData.createtxid))) << OP_CHECKSIG)); // failure marker

                        rawtx = FinalizeCCTx(0, cp, mtx, minerpk, txfee, MarmaraEncodeLoopSettlementOpret(loopData.version, false, loopData.createtxid, loopData.pk, -remaining), false);  //some remainder left
                        if (rawtx.empty()) {
                            result.push_back(Pair("result", "error"));
                            result.push_back(Pair("error", "couldnt finalize CCtx"));
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "FinalizeCCTx error bad partial settlement mtx=" << HexStr(E_MARSHAL(ss << mtx)) << std::endl);
                        }
                        else {
                            result.push_back(Pair("result", "warning"));
                            result.push_back(Pair("warning", "insufficient funds in loop, partial settlement"));
                            result.push_back(Pair("hex", rawtx));
                            result.push_back(Pair("remaining", ValueFromAmount(remaining)));
                            settlementTx = mtx;
                        }
                    }
                    else
                    {
                        // jl777: maybe fund a txfee to report no funds avail
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "no lcl funds available at all"));
                    }
                    //}
                    /*else
                    {
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", "this node does not have the baton"));
                        result.push_back(Pair("myCCaddr", myCCaddr));
                        result.push_back(Pair("batonCCaddr", batonCCaddr));
                    }*/
                }
                else
                {
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", "couldnt get batontxid opret"));
                }
            }
            else
            {
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", "couldnt find batontxid"));
            }
        }
        else
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "couldnt get credit loop creation data"));
        }
    }
    else
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "couldnt get creditloop for the baton"));
    }
    return(result);
}

// enums credit loops (for the refpk as the issuer or all if null refpk passed)
// calls the callback for pending and closed txids (if MARMARA_LOOP_MARKER_VOUT is passed) or only for pending loops (if MARMARA_OPENCLOSE_VOUT is passed)
// for pending loops calls 'callback' with params batontxid and mature height (or -1 if the loop is closed)
template <class T>
static int32_t enum_credit_loops(int32_t nVoutMarker, struct CCcontract_info *cp, int32_t firstheight, int32_t lastheight, int64_t minamount, int64_t maxamount, const CPubKey &refpk, const std::string &refcurrency, T callback)
{
    char marmaraaddr[KOMODO_ADDRESS_BUFSIZE]; 
    int32_t n = 0; 
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
    CPubKey Marmarapk = GetUnspendable(cp, 0);
    GetCCaddress(cp, marmaraaddr, Marmarapk);
    SetCCunspents(unspentOutputs, marmaraaddr, true);

    // do all txid, conditional on spent/unspent
    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream  << "check on marmara addr=" << marmaraaddr << std::endl);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        CTransaction issuancetx;
        uint256 hashBlock;
        uint256 issuancetxid = it->first.txhash;
        int32_t vout = (int32_t)it->first.index;

        // enum creditloop markers:
        if (vout == nVoutMarker)
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "checking tx as marker on marmara addr txid=" << issuancetxid.GetHex() << " vout=" << vout << std::endl);
            if (myGetTransaction(issuancetxid, issuancetx, hashBlock) && !hashBlock.IsNull())  /* enum issuance txns only in blocks */
            {
                if (!issuancetx.IsCoinBase() && issuancetx.vout.size() > 2 && issuancetx.vout.back().nValue == 0 /*has opreturn?*/)
                {
                    struct SMarmaraCreditLoopOpret loopData;
                    if (MarmaraDecodeLoopOpret(issuancetx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY) == MARMARA_ISSUE)
                    {
                        if (get_loop_creation_data(loopData.createtxid, loopData, MARMARA_OPRET_VERSION_ANY) >= 0)
                        {
                            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found issuance tx txid=" << issuancetxid.GetHex() << std::endl);
                            n++;
                            //assert(!loopData.currency.empty());
                            //assert(loopData.pk.size() != 0);
                            if (loopData.currency == refcurrency && loopData.matures >= firstheight && loopData.matures <= lastheight && loopData.amount >= minamount && loopData.amount <= maxamount && (refpk.size() == 0 || loopData.pk == refpk))
                            {
                                std::vector<uint256> creditloop;
                                uint256 settletxid, batontxid;
                                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "issuance tx is filtered, txid=" << issuancetxid.GetHex() << std::endl);

                                if (skipBadLoop(issuancetxid)) {
                                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "skipped bad issuetx, txid=" << issuancetxid.GetHex() << std::endl);
                                    continue;
                                }

                                CTransaction settletx;
                                CTransaction batontx;
                                uint256 hashBlock;
                                uint8_t funcid;

                                LOCK(cs_main);
                                if (get_settlement_txid(settletxid, issuancetxid) == 0)
                                {
                                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found settle tx for issueancetxid=" << issuancetxid.GetHex() << std::endl);

                                    if (myGetTransaction(settletxid, settletx, hashBlock) /*&& !hashBlock.IsNull()*/ && settletx.vout.size() > 1 &&
                                        (funcid = MarmaraDecodeLoopOpret(settletx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY)) != 0)
                                    {
                                        callback(issuancetx, batontx, settletx, loopData);
                                    }
                                    else 
                                        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "could not get or decode settletx=" << settletxid.GetHex() << " (tx could be in mempool)" << std::endl);
                                }
                                else if (MarmaraGetbatontxid(creditloop, batontxid, issuancetxid) > 0)
                                {
                                    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "found baton tx for issueancetxid=" << issuancetxid.GetHex() << std::endl);

                                    if (myGetTransaction(batontxid, batontx, hashBlock) /*&& !hashBlock.IsNull()*/ && batontx.vout.size() > 1 &&
                                        (funcid = MarmaraDecodeLoopOpret(batontx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY)) != 0)
                                    {
                                        callback(issuancetx, batontx, settletx, loopData);
                                    }
                                    else
                                        LOGSTREAMFN("marmara", CCLOG_INFO, stream << "could not get or decode batontx=" << batontxid.GetHex() << " (baton could be in mempool)" << std::endl);
                                }
                                else
                                    LOGSTREAMFN("marmara", CCLOG_INFO, stream << "error finding baton for issuance txid=" << issuancetxid.GetHex() << " (tx could be in mempool)" << std::endl);
                            }
                        }
                        else
                            LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "error load create tx for createtxid=" << loopData.createtxid.GetHex() << std::endl);
                    }
                    else
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "incorrect funcid for issuancetxid=" << issuancetxid.GetHex() << std::endl);
                }
            }
            else
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "cant get tx on marmara marker addr" << " (is in mempool=" << hashBlock.IsNull() << ") " << " txid=" << issuancetxid.GetHex() << std::endl);
        }
    }
    return(n);
}

// adds to the passed vector the settlement transactions for all matured loops 
// called by the miner
// note that several or even all transactions might not fit into the current block, in this case they will be added on the next new block creation
// TODO: provide reserved space in the created block for at least some settlement transactions
void MarmaraRunAutoSettlement(int32_t height, std::vector<CTransaction> & settlementTransactions)
{
    int64_t totalopen, totalclosed;
    std::vector<uint256> issuances, closed;
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    std::string funcname = __func__;
    CPubKey nullpk;

    int32_t firstheight = 0, lastheight = (1 << 30);
    int64_t minamount = 0, maxamount = (1LL << 60);

    if (IsNotInSync() || IsInitialBlockDownload()) {
        LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "node in sync..." << std::endl);
        return;
    }

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "starting enum open batons" << std::endl);
    enum_credit_loops(MARMARA_OPENCLOSE_VOUT, cp, firstheight, lastheight, minamount, maxamount, nullpk, MARMARA_CURRENCY, 
        [&](const CTransaction &issuancetx, const CTransaction &batontx, const CTransaction &settletx, const SMarmaraCreditLoopOpret &loopData) // loopData is updated with last tx opret
        {
            if (settletx.IsNull() && !batontx.IsNull())  // not settled already
            {
                CTransaction newSettleTx;
                uint256 batontxid = batontx.GetHash();
                //TODO: temp UniValue result legacy code, change to remove UniValue

                if (chainActive.LastTip()->GetHeight() >= loopData.matures + 5)   //check height if matured (allow 5 block delay to prevent use of remote txns sent into mempool)
                {
                    LOGSTREAM("marmara", CCLOG_DEBUG2, stream << funcname << " " << "miner calling settlement for batontxid=" << batontxid.GetHex() << std::endl);

                    // do not call LOCK(cs_main), it is already called in enum_credit_loops
                    // also LOCK(cs_main) is called in MarmaraSettlement but should not create a problem
                    UniValue result = MarmaraSettlement(0, batontxid, newSettleTx);
                    if (result["result"].getValStr() == "success") {
                        LOGSTREAM("marmara", CCLOG_INFO, stream << funcname << " " << "miner created settlement tx=" << newSettleTx.GetHash().GetHex() <<  ", for batontxid=" << batontxid.GetHex() << std::endl);
                        settlementTransactions.push_back(newSettleTx);
                    }
                    else if (result["result"].getValStr() == "warning") {
                        LOGSTREAM("marmara", CCLOG_DEBUG1, stream << funcname << " " << "warning=" << result["warning"].getValStr() << " in settlement for batontxid=" << batontxid.GetHex() << std::endl);
                        settlementTransactions.push_back(newSettleTx);
                    }
                    else {
                        LOGSTREAM("marmara", CCLOG_ERROR, stream << funcname << " " << "error=" << result["error"].getValStr() << " in settlement for batontxid=" << batontxid.GetHex() << std::endl);
                    }
                }
            }
        }
    );
}

// create request tx for issuing or transfer baton (cheque) 
// the first call makes the credit loop creation tx
// txid of returned tx is requesttxid
UniValue MarmaraReceive(const CPubKey &remotepk, int64_t txfee, const CPubKey &senderpk, int64_t amount, const std::string &currency, int32_t matures, int32_t avalcount, uint256 batontxid, bool automaticflag)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ); 
    struct CCcontract_info *cp, C; 
    int64_t requestFee; 
    std::string rawtx;

    uint8_t version = MarmaraIs2020JuneUpdateActive(NULL) ? MARMARA_OPRET_LOOP12_VERSION : MARMARA_OPRET_VERSION_DEFAULT;

    cp = CCinit(&C, EVAL_MARMARA);
    if (txfee == 0)
        txfee = 10000;
    
    if (automaticflag != 0 && (matures & 1) == 0)
        matures++;
    else if (automaticflag == 0 && (matures & 1) != 0)
        matures++;

    CPubKey mypk; 
    bool isRemote = IS_REMOTE(remotepk);
    if (isRemote)
        mypk = remotepk;
    else
        mypk = pubkey2pk(Mypubkey());
    uint256 createtxid = zeroid;
    const char *errorstr = NULL;

    if (batontxid == zeroid) 
    {
        // first time checking parameters
        if (currency != MARMARA_CURRENCY)
            errorstr = "for now, only MARMARA loops are supported";
        else if (amount <= txfee)
            errorstr = "amount must be for more than txfee";
        else if (matures <= chainActive.LastTip()->GetHeight())
            errorstr = "it must mature in the future";
        else if (mypk == senderpk)
            errorstr = "cannot request credit from self";
    }
    else
    {
        if (get_create_txid(createtxid, batontxid, version) < 0)
            errorstr = "cant get createtxid from batontxid (check version)";
    }

    if (createtxid != zeroid) 
    {
        // check original cheque params:
        CTransaction looptx;
        uint256 hashBlock;
        struct SMarmaraCreditLoopOpret loopData;

        if (get_loop_creation_data(createtxid, loopData, version) < 0)
            errorstr = "cannot get loop creation data";
        else if (!myGetTransaction(batontxid, looptx, hashBlock) ||
            hashBlock.IsNull() ||  // not in mempool
            looptx.vout.size() < 1 ||
            MarmaraDecodeLoopOpret(looptx.vout.back().scriptPubKey, loopData, version) == 0)
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "cant get looptx.GetHash()=" << looptx.GetHash().GetHex() << " looptx.vout.size()=" << looptx.vout.size() << " version=" << (int)loopData.version << std::endl);
            errorstr = "cant load previous loop tx or tx in mempool or cant decode tx opreturn data (check version)";
        }
        else if (senderpk != loopData.pk)
            errorstr = "current baton holder does not match the requested sender pk";
        else if (loopData.matures <= chainActive.LastTip()->GetHeight())
            errorstr = "credit loop must mature in the future";
    }

    if (errorstr == NULL)
    {
        if (batontxid != zeroid)
            requestFee = MARMARA_BATON_AMOUNT;
        else 
            requestFee = MARMARA_CREATETX_AMOUNT;  // fee value 20000 for easy identification (?)
        if (AddNormalinputsRemote(mtx, mypk, requestFee + txfee, MARMARA_VINS) > 0)  // always add only from mypk to ensure no false credit request!
        {
            CScript opret;

            mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, requestFee, senderpk));
            if (batontxid.IsNull())
                opret = MarmaraEncodeLoopCreateOpret(version, senderpk, amount, matures, currency);
            else
                opret = MarmaraEncodeLoopRequestOpret(version, createtxid, senderpk);

            rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, opret, false);
            if (rawtx.size() == 0)
                errorstr = "couldnt finalize CCtx";
        }
        else 
            errorstr = "dont have enough normal inputs for requestfee and txfee";
    }
    if (rawtx.size() == 0 || errorstr != 0)
    {
        result.push_back(Pair("result", "error"));
        if (errorstr != 0)
            result.push_back(Pair("error", errorstr));
    }
    else
    {
        result.push_back(Pair("result", "success"));
        result.push_back(Pair("hex", rawtx));
        result.push_back(Pair("funcid", batontxid.IsNull() ? "B" : "R"));
        result.push_back(Pair("createtxid", createtxid.GetHex()));
        if (batontxid != zeroid)
            result.push_back(Pair("batontxid", batontxid.GetHex()));
        result.push_back(Pair("senderpk", HexStr(senderpk)));
        if (batontxid == zeroid) {
            result.push_back(Pair("amount", ValueFromAmount(amount)));
            result.push_back(Pair("matures", static_cast<int64_t>(matures)));
            result.push_back(Pair("currency", currency));
        }
    }
    return(result);
}


static int32_t redistribute_lcl_remainder(CMutableTransaction &mtx, struct CCcontract_info *cp, const std::vector<uint256> &creditloop, uint256 batontxid, CAmount amountToDistribute)
{
    CPubKey Marmarapk; 
    int32_t endorsersNumber = creditloop.size(); // number of endorsers, 0 is createtxid, last is holder
    CAmount inputsum, change;
    std::vector <CPubKey> endorserPubkeys;
    CTransaction createtx;
    uint256 hashBlock, dummytxid;
    uint256 createtxid = creditloop[0];
    struct SMarmaraCreditLoopOpret loopData;

    uint8_t marmarapriv[32];
    Marmarapk = GetUnspendable(cp, marmarapriv);

    if (endorsersNumber < 1)  // nobody to return to
        return 0;

    if (myGetTransaction(createtxid, createtx, hashBlock) && createtx.vout.size() > 1 &&
        MarmaraDecodeLoopOpret(createtx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_DEFAULT) != 0)  // get amount value, redistribute_lcl_remainder is only for ver 1.1
    {
        char lockInLoop1of2addr[KOMODO_ADDRESS_BUFSIZE];
        CPubKey createtxidPk = CCtxidaddr_tweak(NULL, createtxid);
        GetCCaddress1of2(cp, lockInLoop1of2addr, Marmarapk, createtxidPk);  // 1of2 lock-in-loop address 

        // add locked-in-loop utxos:
        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream  << "calling AddMarmaraCCInputs for lock-in-loop addr=" << lockInLoop1of2addr << " adding as possible as amount=" << loopData.amount << std::endl);
        if ((inputsum = AddMarmaraCCInputs(IsMarmaraLockedInLoopVout, mtx, endorserPubkeys, lockInLoop1of2addr, loopData.amount, MARMARA_VINS)) >= loopData.amount / endorsersNumber) 
        {
            if (mtx.vin.size() >= CC_MAXVINS) {// total vin number limit
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream  << "too many vins!" << std::endl);
                return -1;
            }

            if (endorserPubkeys.size() != endorsersNumber) {
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << " internal error not matched endorserPubkeys.size()=" << endorserPubkeys.size() << " endorsersNumber=" << endorsersNumber << " line=" << __LINE__ << std::endl);
                return -1;
            }

            CAmount amountToPkNormal = amountToDistribute / endorsersNumber;
            CAmount amountDistributed = amountToPkNormal * endorsersNumber;

            //for (int32_t i = 1; i < creditloop.size() + 1; i ++)  //iterate through all issuers/endorsers, skip i=0 which is 1st receiver tx, n + 1 is batontxid
            int32_t firstVoutNormal = mtx.vout.size();
            for (const auto &endorserPk : endorserPubkeys)
            {
                mtx.vout.push_back(CTxOut(amountToPkNormal, CScript() << ParseHex(HexStr(endorserPk)) << OP_CHECKSIG));  // coins returned to each previous issuer normal output
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << " sending normal amount=" << amountToPkNormal << " to pk=" << HexStr(endorserPk) << std::endl);
            }

            // distribute round error back to vouts, by 1 sat:
            CAmount errorNormals = amountToDistribute - amountDistributed;
            for (int32_t i = firstVoutNormal; i < mtx.vout.size() && errorNormals != 0; i ++)    
                mtx.vout[i].nValue ++, errorNormals --;

            //change = (inputsum - amountReturned);
            change = (inputsum - amountToDistribute);

            // return change to the lock-in-loop fund, distribute for pubkeys:
            if (change > 0) 
            {
                /* uncomment if the same check above is removed
                if (endorserPubkeys.size() != endorsersNumber) {
                    LOGSTREAMFN("marmara", CCLOG_ERROR, stream  << " internal error not matched endorsersPubkeys.size()=" << endorserPubkeys.size() << " endorsersNumber=" << endorsersNumber << " line=" << __LINE__ << std::endl);
                    return -1;
                } */
                int32_t firstVoutCC = mtx.vout.size();
                CAmount amountToPkCC = change / endorserPubkeys.size();
                CAmount amountDistributedCC = amountToPkCC * endorserPubkeys.size();
                for (const auto &pk : endorserPubkeys) 
                {
                    // each LCL utxo is marked with the pubkey who owns this part of the loop amount
                    // So for staking only those LCL utxo are picked up that are marked with the current node's pubkey
                    CScript opret = MarmaraEncodeLoopCCVoutOpret(createtxid, pk);   // add mypk to vout to identify who has locked coins in the credit loop
                    mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(amountToPkCC, createtxidPk, opret));  // TODO: losing remainder?

                    LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream  << "distributing to loop change/pubkeys.size()=" << amountToPkCC << " cc opret pk=" << HexStr(pk) << std::endl);
                }

                // distribute round error back to vouts, by 1 sat:
                CAmount errorCCOutputs = change - amountDistributedCC;
                for (int32_t i = firstVoutCC; i < mtx.vout.size() && errorCCOutputs != 0; i ++)    
                    mtx.vout[i].nValue ++, errorCCOutputs --;
            }

            CC *lockInLoop1of2cond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, createtxidPk);  
            CCAddVintxCond(cp, lockInLoop1of2cond, marmarapriv); //add probe condition to spend from the lock-in-loop address
            cc_free(lockInLoop1of2cond);
        }
        else  {
            LOGSTREAMFN("marmara", CCLOG_ERROR, stream  << "couldnt get locked-in-loop amount to return to endorsers" << std::endl);
            return -1;
        }
    }
    else {
        LOGSTREAMFN("marmara", CCLOG_ERROR, stream  << "could not load createtx" << std::endl);
        return -1;
    }
    return 0;
}


// issue or transfer coins to the next receiver
UniValue MarmaraIssue(const CPubKey &remotepk, int64_t txfee, uint8_t funcid, const CPubKey &receiverpk, const struct SMarmaraOptParams &optParams, uint256 requesttxid, uint256 batontxid)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ); 
    std::string rawtx; 
    std::string errorStr;
    uint256 createtxid, hashBlock;
    CTransaction dummytx;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    if (txfee == 0)
        txfee = 10000;

    uint8_t version = MarmaraIs2020JuneUpdateActive(NULL) ? MARMARA_OPRET_LOOP12_VERSION : MARMARA_OPRET_VERSION_DEFAULT;  // version 2 matches the whole project version 1.2

    uint8_t marmarapriv[32];
    CPubKey Marmarapk = GetUnspendable(cp, marmarapriv);
    CPubKey mypk;
    bool isRemote = IS_REMOTE(remotepk);
    if (isRemote)
        mypk = remotepk;
    else
        mypk = pubkey2pk(Mypubkey());
    
    if (mypk == receiverpk)
        errorStr = "cannot send baton to self";  // check it here
    else if (!myGetTransaction(requesttxid, dummytx, hashBlock) || hashBlock.IsNull())
        errorStr = "can't get requesttxid (requesttxid might be still in mempool)";
    else if (get_create_txid(createtxid, requesttxid, version) < 0)
        errorStr = "can't get createtxid from requesttxid (check version)";
    else if (check_request_tx(requesttxid, receiverpk, funcid, version, errorStr))
    {
        struct SMarmaraCreditLoopOpret loopData;

        if (get_loop_creation_data(createtxid, loopData, version) >= 0)
        {
            if (version != loopData.version)    {
                errorStr = "incompatible loop version";
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", errorStr));
                return result;
            }

            uint256 dummytxid;
            std::vector<uint256> creditloop;
            LOCK(cs_main);
            int32_t endorsersNumber = MarmaraGetbatontxid(creditloop, dummytxid, requesttxid);

            int32_t height = get_next_height();
            if (height > 0 && height < MARMARA_POS_IMPROVEMENTS_HEIGHT && endorsersNumber >= 2) {
                errorStr = "endorser number >= 3 allowed after hardfork";
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", errorStr));
                return result;
            }

            if (endorsersNumber < 0 )      {
                errorStr = "incorrect requesttxid, could not get endorsers";
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", errorStr));
                return result;
            }
            if (endorsersNumber >= MARMARA_MAXENDORSERS) {
                errorStr = "too many endorsers";
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", errorStr));
                return result;
            }

            char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
            int64_t inputsum = 0;
            std::vector<CPubKey> pubkeys;
            int64_t amountToLock;
            if (version == 1)
                amountToLock = (endorsersNumber > 0 ? loopData.amount / (endorsersNumber + 1) : loopData.amount);  // include new endorser
            else
                amountToLock = loopData.amount;

            GetCCaddress1of2(cp, activated1of2addr, Marmarapk, mypk);  // 1of2 address where the activated endorser's money is locked

            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << "calling AddMarmaraCCInputs for activated addr=" << activated1of2addr << " needs activated amount to lock-in-loop=" << amountToLock << std::endl);
            if (version == 1 || version == 2 && funcid == MARMARA_ISSUE)    {
                inputsum = AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, amountToLock, MARMARA_VINS);
                if (inputsum < amountToLock)    {
                    errorStr = "don't have enough activated inputs for amount";
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", errorStr));
                    return result;
                }
            }
            
            mtx.vin.push_back(CTxIn(requesttxid, MARMARA_REQUEST_VOUT, CScript()));  // spend the request tx baton, will add 20000 for marmaraissue or 10000 for marmaratransfer
            if (funcid == MARMARA_TRANSFER)
                mtx.vin.push_back(CTxIn(batontxid, MARMARA_BATON_VOUT, CScript()));   // for marmaratransfer spend the previous baton (+ 10000 for marmaratransfer)

            if (funcid == MARMARA_ISSUE)  // add two more txfee for marmaraissue
            {
                if (AddNormalinputs(mtx, mypk, txfee + MARMARA_LOOP_MARKER_AMOUNT, MARMARA_VINS, isRemote) <= 0)     {
                    errorStr = "dont have enough normal inputs for txfee";
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", errorStr));
                    return result;
                }
            }

            mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, MARMARA_BATON_AMOUNT, receiverpk));  // vout0 is transfer of baton to the next receiver (-txfee for marmaraissue and marmaratransfer)
            if (funcid == MARMARA_ISSUE)
                mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, MARMARA_LOOP_MARKER_AMOUNT, Marmarapk));  // vout1 is marker in issuance tx to list all loops

            // get createtxid pk for 1of2 loop cc vout
            CPubKey createtxidPk = CCtxidaddr_tweak(NULL, createtxid);

            // add cc lock-in-loop opret 
            // mark opret with my pk to indicate whose vout it is (to add it as mypk staking utxo) 
            CScript lockOpret = MarmaraEncodeLoopCCVoutOpret(createtxid, loopData.pk);
            // lock 1/N amount for version 1 or 1/2 amount for version 2 in loop:
            // add cc opret with mypk to cc vout 
            CAmount utxoAmount = version == 1 ? amountToLock : amountToLock / 2;
            LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "sending to loop amount=" << utxoAmount << " marked with issuerpk=" << HexStr(loopData.pk) << std::endl);
            mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(utxoAmount, createtxidPk, lockOpret)); //vout2 is issued amount

            if (funcid == MARMARA_ISSUE)
                mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, MARMARA_OPEN_MARKER_AMOUNT, Marmarapk));  // vout3 is open/close marker in issuance tx

            if (version == 2)   {
                // add holder utxo 1/2 amount
                CScript opretReceiver = MarmaraEncodeLoopCCVoutOpret(createtxid, receiverpk);
                // add cc opret with receiver to cc vout 
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "sending to loop amount=" << utxoAmount << " marked with receiverpk=" << HexStr(receiverpk) << std::endl);
                mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(utxoAmount, createtxidPk, opretReceiver)); //vout2 is issued amount
            }

            // return CC change to mypk activated address:
            CAmount CCchange = (inputsum - amountToLock);
            if (CCchange > 0)  // should not be change > 0 for transfers for ver 1.2
            {
                int32_t height = komodo_nextheight();
                if ((height & 1) != 0) // make height even as only even height is considered for staking (TODO: strange)
                    height++;
                CScript opret = MarmaraEncodeCoinbaseOpret(MARMARA_ACTIVATED, mypk, height);
                // add coinbase opret to ccvout for the change
                mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(CCchange, mypk, opret));  // adding MarmaraCoinbase cc vout 'opret' for change
            }

            if (version == 1)   {
                if (endorsersNumber >= 1)   {
                    if (redistribute_lcl_remainder(mtx, cp, creditloop, batontxid, amountToLock) < 0)  {// if there are issuers already then distribute and return amount / n value
                        errorStr = "could not return locked in loop funds to endorsers";
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", errorStr));
                        return result;
                    }
                }
            }
            else {  // version == 2
                if (funcid == MARMARA_TRANSFER) {
                    char lockInLoop1of2addr[KOMODO_ADDRESS_BUFSIZE];
                    CPubKey createtxidPk = CCtxidaddr_tweak(NULL, createtxid);
                    GetCCaddress1of2(cp, lockInLoop1of2addr, Marmarapk, createtxidPk);
                    std::vector <CPubKey> endorserPubkeys;

                    CAmount inputsum = AddMarmaraCCInputs(IsMarmaraLockedInLoopVout, mtx, endorserPubkeys, lockInLoop1of2addr, loopData.amount, MARMARA_VINS); 
                    if (llabs(inputsum - loopData.amount) > MARMARA_LOOP_TOLERANCE)  {
                        errorStr = "invalid amount locked in loop";
                        result.push_back(Pair("result", "error"));
                        result.push_back(Pair("error", errorStr));
                        return result;
                    }
                }
            }
            
            if (version == 1 || funcid == MARMARA_ISSUE) {  // for ver 1.2 only issue has actvated 
                CC* activated1of2cond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, mypk);  // create vintx probe 1of2 cond to spend from activated account
                CCAddVintxCond(cp, activated1of2cond);      // add the probe to cp, it is copied and we can cc_free it
                cc_free(activated1of2cond);
            }
            if (version == 2 || funcid == MARMARA_TRANSFER)   {
                CC *lockInLoop1of2cond = MakeCCcond1of2(EVAL_MARMARA, Marmarapk, createtxidPk);  
                CCAddVintxCond(cp, lockInLoop1of2cond, marmarapriv); //add probe condition to spend from the lock-in-loop address
                cc_free(lockInLoop1of2cond);
            }

            CScript opret;
            // NOTE: for loops version 1.2 (endorser do not add coins to loop and simply transfer the baton)
            // we add version==2 to the last vout opreturn
            // but loop vout cc vdata still has version==1  
            if (funcid == MARMARA_ISSUE)
                opret = MarmaraEncodeLoopIssuerOpret(version, createtxid, receiverpk, optParams.autoSettlement, optParams.autoInsurance, optParams.avalCount, optParams.disputeExpiresOffset, optParams.escrowOn, optParams.blockageAmount);
            else
                opret = MarmaraEncodeLoopTransferOpret(version, createtxid, receiverpk, optParams.avalCount);

            rawtx = FinalizeCCTx(0, cp, mtx, mypk, txfee, opret, false);

            if (rawtx.size() == 0) {
                errorStr = "couldnt finalize tx";
                LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "couldnt finalize, bad mtx=" << HexStr(E_MARSHAL(ss << mtx)) << std::endl);
            }
        }
        else
        {
            errorStr = "cannot get loop creation data";
        }
    }
    if (!errorStr.empty())
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", errorStr));
    }
    else
    {
        result.push_back(Pair("result", "success"));
        result.push_back(Pair("hex", rawtx));
        char sfuncid[2]; 
        sfuncid[0] = funcid;
        sfuncid[1] = '\0';
        result.push_back(Pair("funcid", sfuncid));
        result.push_back(Pair("createtxid", createtxid.GetHex()));
        result.push_back(Pair("requesttxid", requesttxid.GetHex()));
        if (funcid == MARMARA_TRANSFER)
            result.push_back(Pair("batontxid", batontxid.GetHex()));
        result.push_back(Pair("receiverpk", HexStr(receiverpk)));
//        result.push_back(Pair("amount", ValueFromAmount(amount)));
//        result.push_back(Pair("matures", static_cast<int64_t>(matures)));
//        result.push_back(Pair("currency", currency));
    }
    return(result);
}

UniValue MarmaraCreditloop(const CPubKey & remotepk, uint256 txid)
{
    UniValue result(UniValue::VOBJ), a(UniValue::VARR); 
    std::vector<uint256> creditloop; 
    uint256 batontxid, hashBlock; 
    uint8_t funcid; 
    int32_t numerrs = 0, n; 
    CTransaction lasttx; 
    struct CCcontract_info *cp, C;
    struct SMarmaraCreditLoopOpret loopData;
    bool isSettledOk = false;

    CPubKey mypk;
    if (IS_REMOTE(remotepk))
        mypk = remotepk;
    else
        mypk = pubkey2pk(Mypubkey());

    cp = CCinit(&C, EVAL_MARMARA);
    LOCK(cs_main);
    if ((n = MarmaraGetbatontxid(creditloop, batontxid, txid)) > 0)
    {
        if (get_loop_creation_data(creditloop[0], loopData, MARMARA_OPRET_VERSION_ANY) == 0)
        {
            uint256 issuetxid, settletxid, lasttxid;
            int32_t vini, height;

            if (n > 1)
                issuetxid = creditloop[1];
            else
                issuetxid = batontxid;

            std::vector<uint256> looptxids (creditloop.begin(), creditloop.end());

            if (get_settlement_txid(settletxid, issuetxid) == 0)
            {
                // loop is closed - last tx is the settle tx
                lasttxid = settletxid;
                looptxids.push_back(batontxid);  // add baton to to add its info to the result too
            }
            else
            {
                // loop is not closed - last tx is the baton
                lasttxid = batontxid;
            }

            // add last tx info
            if (myGetTransaction(lasttxid, lasttx, hashBlock) && lasttx.vout.size() > 1)
            {
                char normaladdr[KOMODO_ADDRESS_BUFSIZE], myCCaddr[KOMODO_ADDRESS_BUFSIZE], vout0addr[KOMODO_ADDRESS_BUFSIZE], batonCCaddr[KOMODO_ADDRESS_BUFSIZE];
                vuint8_t vmypk(mypk.begin(), mypk.end());

                result.push_back(Pair("result", "success"));
                Getscriptaddress(normaladdr, CScript() << ParseHex(HexStr(vmypk)) << OP_CHECKSIG);
                result.push_back(Pair("myNormalAddress", normaladdr));
                GetCCaddress(cp, myCCaddr, vmypk);
                result.push_back(Pair("myCCaddress", myCCaddr));

                if ((funcid = MarmaraDecodeLoopOpret(lasttx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY)) != 0)
                {
                    result.push_back(Pair("version", (int)loopData.version));
                    std::string sfuncid(1, (char)funcid);
                    result.push_back(Pair("funcid", sfuncid));
                    result.push_back(Pair("currency", loopData.currency));

                    if (loopData.createtxid != creditloop[0])
                    {
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "invalid loopData.createtxid for creditloop[0]=" << creditloop[0].GetHex() << " " << std::endl);
                        result.push_back(Pair("incorrect-createtxid-in-baton-opret", loopData.createtxid.GetHex()));
                        numerrs++;
                    }

                    if (funcid == MARMARA_SETTLE) //settled okay
                    {
                        //refcreatetxid = creditloop[0];
                        result.push_back(Pair("settlement", settletxid.GetHex()));
                        result.push_back(Pair("createtxid", creditloop[0].GetHex()));
                        result.push_back(Pair("remainder", ValueFromAmount(loopData.remaining)));
                        result.push_back(Pair("matures", static_cast<int64_t>(loopData.matures)));  // used true "height" instead
                        result.push_back(Pair("pubkey", HexStr(loopData.pk)));
                        Getscriptaddress(normaladdr, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG);
                        result.push_back(Pair("settledToNormalAddress", normaladdr));
                        result.push_back(Pair("collected", ValueFromAmount(lasttx.vout[0].nValue)));
                        Getscriptaddress(vout0addr, lasttx.vout[0].scriptPubKey);
                        if (strcmp(normaladdr, vout0addr) != 0)
                        {
                            result.push_back(Pair("incorrect-vout0-address-not-matched-pk-in-opret", vout0addr));
                            numerrs++;
                        }
                        isSettledOk = true;
                    }
                    else if (funcid == MARMARA_SETTLE_PARTIAL)  //settled partially
                    {
                        //refcreatetxid = creditloop[0];
                        result.push_back(Pair("settlement", settletxid.GetHex()));
                        result.push_back(Pair("createtxid", creditloop[0].GetHex()));
                        result.push_back(Pair("remainder", ValueFromAmount(loopData.remaining)));
                        result.push_back(Pair("matures", static_cast<int64_t>(loopData.matures))); // used true "height" instead
                        Getscriptaddress(vout0addr, lasttx.vout[0].scriptPubKey);
                        result.push_back(Pair("txidaddr", vout0addr));  //TODO: why 'txidaddr'?
                        if (lasttx.vout.size() > 0)
                            result.push_back(Pair("collected", ValueFromAmount(lasttx.vout[0].nValue)));
                    }
                    else
                    {
                        result.push_back(Pair("batontxid", batontxid.GetHex()));
                        result.push_back(Pair("createtxid", creditloop[0].GetHex()));
                        result.push_back(Pair("amount", ValueFromAmount(loopData.amount)));
                        result.push_back(Pair("matures", static_cast<int64_t>(loopData.matures)));
                        result.push_back(Pair("batonpk", HexStr(loopData.pk)));
                        Getscriptaddress(normaladdr, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG);
                        result.push_back(Pair("batonaddr", normaladdr));
                        GetCCaddress(cp, batonCCaddr, loopData.pk);  // baton address
                        result.push_back(Pair("batonCCaddr", batonCCaddr));
                        Getscriptaddress(vout0addr, lasttx.vout[0].scriptPubKey);
                        if (strcmp(vout0addr, batonCCaddr) != 0)  // TODO: how is this possible?
                        {
                            result.push_back(Pair("incorrect-vout0-address-not-matched-baton-address", normaladdr));
                            numerrs++;
                        }

                        if (strcmp(myCCaddr, /*normaladdr*/batonCCaddr) == 0) // TODO: impossible with normal addr
                            result.push_back(Pair("ismine", static_cast<int64_t>(1)));
                        else
                            result.push_back(Pair("ismine", static_cast<int64_t>(0)));
                    }
                    result.push_back(Pair("height", static_cast<int64_t>(get_block_height(hashBlock))));

                }
                else
                {
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", "couldnt decode last tx opret"));
                    return result;
                }
            }
            else
            {
                result.push_back(Pair("result", "error"));
                result.push_back(Pair("error", "couldnt load last tx or incorrect last tx"));
                return result;
            }
            
            // add locked-in-loop amount:
            char lockInLoop1of2addr[KOMODO_ADDRESS_BUFSIZE];
            CPubKey createtxidPk = CCtxidaddr_tweak(NULL, creditloop[0]);
            GetCCaddress1of2(cp, lockInLoop1of2addr, GetUnspendable(cp, NULL), createtxidPk);  // 1of2 lock-in-loop address 
            std::vector<CPubKey> pubkeys;
            CMutableTransaction mtx;

            int64_t amountLockedInLoop = AddMarmaraCCInputs(IsMarmaraLockedInLoopVout, mtx, pubkeys, lockInLoop1of2addr, 0, 0);
            result.push_back(Pair("LockedInLoopCCaddr", lockInLoop1of2addr));
            result.push_back(Pair("LockedInLoopAmount", ValueFromAmount(amountLockedInLoop)));  // should be 0 if 

            // add credit loop data:
            for (int32_t i = 0; i < looptxids.size(); i++)
            {
                if (myGetTransaction(looptxids[i], lasttx, hashBlock) != 0 && lasttx.vout.size() > 1)
                {
                    //uint256 createtxid = zeroid;
                    if ((funcid = MarmaraDecodeLoopOpret(lasttx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY)) != 0)
                    {
                        char normaladdr[KOMODO_ADDRESS_BUFSIZE], ccaddr[KOMODO_ADDRESS_BUFSIZE], vout0addr[KOMODO_ADDRESS_BUFSIZE];

                        UniValue obj(UniValue::VOBJ);
                        obj.push_back(Pair("version", (int)loopData.version));
                        obj.push_back(Pair("txid", looptxids[i].GetHex()));
                        std::string sfuncid(1, (char)funcid);
                        obj.push_back(Pair("funcid", sfuncid));
                        if (funcid == MARMARA_REQUEST || funcid == MARMARA_CREATELOOP)
                        {
                            //createtxid = looptxids[i];
                            obj.push_back(Pair("issuerpk", HexStr(loopData.pk)));
                            Getscriptaddress(normaladdr, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG);
                            obj.push_back(Pair("issuerNormalAddress", normaladdr));
                            GetCCaddress(cp, ccaddr, loopData.pk);
                            obj.push_back(Pair("issuerCCAddress", ccaddr));
                        }
                        else
                        {
                            obj.push_back(Pair("receiverpk", HexStr(loopData.pk)));
                            Getscriptaddress(normaladdr, CScript() << ParseHex(HexStr(loopData.pk)) << OP_CHECKSIG);
                            obj.push_back(Pair("receiverNormalAddress", normaladdr));
                            GetCCaddress(cp, ccaddr, loopData.pk);
                            obj.push_back(Pair("receiverCCAddress", ccaddr));
                        }
                        obj.push_back(Pair("height", static_cast<int64_t>(get_block_height(hashBlock))));
                        Getscriptaddress(vout0addr, lasttx.vout[0].scriptPubKey);
                        /*if (strcmp(vout0addr, normaladdr) != 0)  
                        {
                            obj.push_back(Pair("incorrect-vout0address", vout0addr));
                            numerrs++;
                        }*/
                        /*if (i == 0 && isSettledOk)  // why isSettledOk checked?..
                        {
                            result.push_back(Pair("amount", ValueFromAmount(loopData.amount)));
                            result.push_back(Pair("matures", static_cast<int64_t>(loopData.matures)));
                        }*/
                        /* not relevant now as we do not copy params to new oprets
                        if (createtxid != refcreatetxid || amount != refamount || matures != refmatures || currency != refcurrency)
                        {
                        numerrs++;
                        obj.push_back(Pair("objerror", (char *)"mismatched createtxid or amount or matures or currency"));
                        obj.push_back(Pair("createtxid", createtxid.GetHex()));
                        obj.push_back(Pair("amount", ValueFromAmount(amount)));
                        obj.push_back(Pair("matures", static_cast<int64_t>(matures)));
                        obj.push_back(Pair("currency", currency));
                        } */
                        a.push_back(obj);
                    }
                }
            }
            result.push_back(Pair("n", static_cast<int64_t>(n)));
//            result.push_back(Pair("numerrors", static_cast<int64_t>(numerrs)));
            result.push_back(Pair("creditloop", a));

        }
        else
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "couldnt get loop creation data"));
        }
    }
    else if (n == 0)
    {
        // output info of createtx if only createtx exists
        if (get_loop_creation_data(txid, loopData, MARMARA_OPRET_VERSION_ANY) == 0)
        {
            result.push_back(Pair("version", loopData.version));
            std::string sfuncid(1, (char)loopData.lastfuncid);
            result.push_back(Pair("funcid", sfuncid));
            result.push_back(Pair("currency", loopData.currency));
            result.push_back(Pair("amount", ValueFromAmount(loopData.amount)));
            result.push_back(Pair("matures", static_cast<int64_t>(loopData.matures)));
            result.push_back(Pair("issuerpk", HexStr(loopData.pk)));
            result.push_back(Pair("createtxid", txid.GetHex()));
        }
        else
        {
            result.push_back(Pair("result", "error"));
            result.push_back(Pair("error", "couldnt get loop creation data"));
        }
    }
    else
    {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "couldnt get creditloop"));
    }
    return(result);
}

// collect miner pool rewards (?)
UniValue MarmaraPoolPayout(int64_t txfee, int32_t firstheight, double perc, char *jsonstr) // [[pk0, shares0], [pk1, shares1], ...]
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    UniValue result(UniValue::VOBJ), a(UniValue::VARR); 
    cJSON *item, *array; std::string rawtx; 
    int32_t i, n; 
    uint8_t buf[CPubKey::COMPRESSED_PUBLIC_KEY_SIZE]; 
    CPubKey Marmarapk, pk, poolpk; 
    int64_t payout, poolfee = 0, total, totalpayout = 0; 
    double poolshares, share, shares = 0.; 
    char *pkstr;
    const char *errorstr = 0;
    struct CCcontract_info *cp, C;

    poolpk = pubkey2pk(Mypubkey());
    if (txfee == 0)
        txfee = 10000;
    cp = CCinit(&C, EVAL_MARMARA);
    Marmarapk = GetUnspendable(cp, 0);
    if ((array = cJSON_Parse(jsonstr)) != 0 && (n = cJSON_GetArraySize(array)) > 0)
    {
        for (i = 0; i < n; i++)
        {
            item = jitem(array, i);
            if ((pkstr = jstr(jitem(item, 0), 0)) != 0 && strlen(pkstr) == (2 * CPubKey::COMPRESSED_PUBLIC_KEY_SIZE))
                shares += jdouble(jitem(item, 1), 0);
            else
            {
                errorstr = "all items must be of the form [<pubkey>, <shares>]";
                break;
            }
        }
        if (errorstr == 0 && shares > SMALLVAL)
        {
            shares += shares * perc;
            if ((total = AddMarmaraCoinbases(cp, mtx, firstheight, poolpk, 60)) > 0)
            {
                for (i = 0; i < n; i++)
                {
                    item = jitem(array, i);
                    if ((share = jdouble(jitem(item, 1), 0)) > SMALLVAL)
                    {
                        payout = (share * (total - txfee)) / shares;
                        if (payout > 0)
                        {
                            if ((pkstr = jstr(jitem(item, 0), 0)) != 0 && strlen(pkstr) == (2 * CPubKey::COMPRESSED_PUBLIC_KEY_SIZE))
                            {
                                UniValue x(UniValue::VOBJ);
                                totalpayout += payout;
                                decode_hex(buf, CPubKey::COMPRESSED_PUBLIC_KEY_SIZE, pkstr);
                                mtx.vout.push_back(MakeCC1of2vout(EVAL_MARMARA, payout, Marmarapk, buf2pk(buf)));
                                x.push_back(Pair(pkstr, (double)payout / COIN));
                                a.push_back(x);
                            }
                        }
                    }
                }
                if (totalpayout > 0 && total > totalpayout - txfee)
                {
                    poolfee = (total - totalpayout - txfee);
                    mtx.vout.push_back(MakeCC1of2vout(EVAL_MARMARA, poolfee, Marmarapk, poolpk));
                }
                rawtx = FinalizeCCTx(0, cp, mtx, poolpk, txfee, MarmaraEncodeCoinbaseOpret(MARMARA_POOL, poolpk, firstheight), false);
                if (rawtx.size() == 0)
                    errorstr = "couldnt finalize CCtx";
            }
            else errorstr = "couldnt find any coinbases to payout";
        }
        else if (errorstr == 0)
            errorstr = "no valid shares submitted";
        free(array);
    }
    else errorstr = "couldnt parse poolshares jsonstr";
    if (rawtx.size() == 0 || errorstr != 0)
    {
        result.push_back(Pair("result", "error"));
        if (errorstr != 0)
            result.push_back(Pair("error", errorstr));
    }
    else
    {
        result.push_back(Pair("result", "success"));
        result.push_back(Pair("hex", rawtx));
        if (totalpayout > 0 && total > totalpayout - txfee)
        {
            result.push_back(Pair("firstheight", static_cast<int64_t>(firstheight)));
            result.push_back(Pair("lastheight", static_cast<int64_t>(((firstheight / MARMARA_GROUPSIZE) + 1) * MARMARA_GROUPSIZE - 1)));
            result.push_back(Pair("total", ValueFromAmount(total)));
            result.push_back(Pair("totalpayout", ValueFromAmount(totalpayout)));
            result.push_back(Pair("totalshares", shares));
            result.push_back(Pair("poolfee", ValueFromAmount(poolfee)));
            result.push_back(Pair("perc", ValueFromAmount((int64_t)(100. * (double)poolfee / totalpayout * COIN))));
            result.push_back(Pair("payouts", a));
        }
    }
    return(result);
}

// list loops, open[] and closed[], for all pks or specific pk 
UniValue MarmaraInfo(const CPubKey &refpk, int32_t firstheight, int32_t lastheight, int64_t minamount, int64_t maxamount, const std::string &currencyparam)
{
    CMutableTransaction mtx; 
    std::string currency;
    std::vector<CPubKey> pubkeys;
    UniValue result(UniValue::VOBJ), a(UniValue::VARR), b(UniValue::VARR); 
    CAmount totalclosed = 0LL, totalopen = 0LL; 
    std::vector<uint256> issuances, closed; 
    CPubKey Marmarapk; 
    char mynormaladdr[KOMODO_ADDRESS_BUFSIZE];
    char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
    char myccaddr[KOMODO_ADDRESS_BUFSIZE];
    bool isRemote = false;
    
    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    Marmarapk = GetUnspendable(cp, 0);
    result.push_back(Pair("result", "success"));

    if (refpk.IsValid())
    {
        vuint8_t vrefpk(refpk.begin(), refpk.end());

        Getscriptaddress(mynormaladdr, CScript() << ParseHex(HexStr(vrefpk)) << OP_CHECKSIG);
        result.push_back(Pair("myNormalAddress", mynormaladdr));
        result.push_back(Pair("myPubkeyNormalAmount", ValueFromAmount(CCaddress_balance(mynormaladdr, 0, true)))); // show utxo in mempool to match pWalletMain->GetBalance()
        if (!isRemote && pwalletMain && pwalletMain->HaveKey(refpk.GetID())) { // show wallet balance if refpk is mine
            LOCK2(cs_main, pwalletMain->cs_wallet);
            result.push_back(Pair("myWalletNormalAmount", ValueFromAmount(pwalletMain->GetBalance())));
        }

        GetCCaddress1of2(cp, activated1of2addr, Marmarapk, vrefpk);
        result.push_back(Pair("myCCActivatedAddress", activated1of2addr));

        // show only confirmed:
        result.push_back(Pair("myActivatedAmount", ValueFromAmount(AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, 0, MARMARA_VINS))));
        result.push_back(Pair("myTotalAmountOnActivatedAddress", ValueFromAmount(CCaddress_balance(activated1of2addr, 1))));

        GetCCaddress(cp, myccaddr, vrefpk);
        result.push_back(Pair("myCCAddress", myccaddr));
        result.push_back(Pair("myCCBalance", ValueFromAmount(CCaddress_balance(myccaddr, 1))));
    }

    // calc lock-in-loops amount for refpk:
    CAmount loopAmount = 0;
    CAmount totalLoopAmount = 0;
    char prevloopaddr[KOMODO_ADDRESS_BUFSIZE] = "";
    UniValue resultloops(UniValue::VARR);
    EnumLockedInLoop(
        [&](char *loopaddr, const CTransaction & tx, int32_t nvout, CBlockIndex *pindex) // call enumerator with callback
        {
            if (strcmp(prevloopaddr, loopaddr) != 0)   // loop address changed
            {
                if (prevloopaddr[0] != '\0')   // prevloop was
                {
                    UniValue entry(UniValue::VOBJ);
                    // if new loop then store amount for the prevloop
                    entry.push_back(Pair("LoopAddress", prevloopaddr));
                    entry.push_back(Pair("myAmountLockedInLoop", ValueFromAmount(loopAmount)));
                    resultloops.push_back(entry);
                    loopAmount = 0;  //reset for the next loop
                }
                strcpy(prevloopaddr, loopaddr);
            }
            loopAmount += tx.vout[nvout].nValue;
            totalLoopAmount += tx.vout[nvout].nValue;
        },
        refpk
    );

    if (prevloopaddr[0] != '\0') {   // last loop
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("LoopAddress", prevloopaddr));
        entry.push_back(Pair("myAmountLockedInLoop", ValueFromAmount(loopAmount)));
        resultloops.push_back(entry);
        //std::cerr << "lastloop " << " prevloopaddr=" << prevloopaddr << " loopAmount=" << loopAmount << std::endl;
    }
    result.push_back(Pair("Loops", resultloops));
    result.push_back(Pair("TotalLockedInLoop", ValueFromAmount(totalLoopAmount)));

    if (refpk.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE)
        result.push_back(Pair("issuer", HexStr(refpk)));
    if (currencyparam.empty())
        currency = (char *)MARMARA_CURRENCY;
    else
        currency = currencyparam;
    // not sure about logic of this, changed:
    //if (firstheight <= lastheight)
    //    firstheight = 0, lastheight = INT32_MAX;
    //if (minamount <= maxamount)
    //    minamount = 0, maxamount = LLONG_MAX;
    if (lastheight == 0)    {
        firstheight = 0;
        lastheight = INT32_MAX;
    }    
    if (maxamount == 0) {
        minamount = 0;
        maxamount = LLONG_MAX;
    }
    result.push_back(Pair("firstheight", static_cast<int64_t>(firstheight)));
    result.push_back(Pair("lastheight", static_cast<int64_t>(lastheight)));
    result.push_back(Pair("minamount", ValueFromAmount(minamount)));
    result.push_back(Pair("maxamount", ValueFromAmount(maxamount)));
    result.push_back(Pair("currency", currency));

    totalopen = 0LL;
    totalclosed = 0LL;
    enum_credit_loops(MARMARA_LOOP_MARKER_VOUT, cp, firstheight, lastheight, minamount, maxamount, refpk, currency, 
        [&](const CTransaction &issuancetx, const CTransaction &batontx, const CTransaction &settletx, const SMarmaraCreditLoopOpret &loopData) 
        {
            if (settletx.IsNull())  {
                issuances.push_back(issuancetx.GetHash());
                totalopen += loopData.amount;
            }
            else {
                closed.push_back(issuancetx.GetHash());
                totalclosed += loopData.amount;
            }
        });
    
    result.push_back(Pair("n", static_cast<int64_t>(issuances.size() + closed.size())));
    result.push_back(Pair("numpending", static_cast<int64_t>(issuances.size())));
    for (int32_t i = 0; i < issuances.size(); i++)
        a.push_back(issuances[i].GetHex());
    result.push_back(Pair("issuances", a));
    result.push_back(Pair("totalamount", ValueFromAmount(totalopen)));
    result.push_back(Pair("numclosed", static_cast<int64_t>(closed.size())));
    for (int32_t i = 0; i < closed.size(); i++)
        b.push_back(closed[i].GetHex());
    result.push_back(Pair("closed", b));
    result.push_back(Pair("totalclosed", ValueFromAmount(totalclosed)));
    
    return(result);
}

// list loops, open[] and closed[], for the holder pk
UniValue MarmaraHolderLoops(const CPubKey &refpk, int32_t firstheight, int32_t lastheight, int64_t minamount, int64_t maxamount, const std::string &currencyparam)
{
    CMutableTransaction mtx; 
    std::string currency;
    UniValue result(UniValue::VOBJ), a(UniValue::VARR), b(UniValue::VARR); 
    CAmount totalclosed = 0LL, totalopen = 0LL; 
    std::vector<uint256> issuances, closed; 
    CPubKey nullpk;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);

    if (refpk.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE)
        result.push_back(Pair("holder", HexStr(refpk)));
    if (currencyparam.empty())
        currency = MARMARA_CURRENCY;
    else
        currency = currencyparam;
    // not sure about logic of this, changed:
    //if (firstheight <= lastheight)
    //    firstheight = 0, lastheight = INT32_MAX;
    //if (minamount <= maxamount)
    //    minamount = 0, maxamount = LLONG_MAX;
    if (lastheight == 0)    {
        firstheight = 0;
        lastheight = INT32_MAX;
    }    
    if (maxamount == 0) {
        minamount = 0;
        maxamount = LLONG_MAX;
    }
    result.push_back(Pair("firstheight", static_cast<int64_t>(firstheight)));
    result.push_back(Pair("lastheight", static_cast<int64_t>(lastheight)));
    result.push_back(Pair("minamount", ValueFromAmount(minamount)));
    result.push_back(Pair("maxamount", ValueFromAmount(maxamount)));
    result.push_back(Pair("currency", currency));

    enum_credit_loops(MARMARA_LOOP_MARKER_VOUT, cp, firstheight, lastheight, minamount, maxamount, nullpk, currency, 
        [&](const CTransaction &issuancetx, const CTransaction &batontx, const CTransaction &settletx, const SMarmaraCreditLoopOpret &loopData) 
        {
            // std::cerr << __func__ << " issuancetx=" << issuancetx.GetHash().GetHex() << " loopData.pk=" << HexStr(loopData.pk) << " refpk=" << HexStr(refpk) << std::endl;
            if (loopData.pk == refpk)   {  // loopData is updated with last loop baton or settle tx
                if (settletx.IsNull())  {
                    issuances.push_back(issuancetx.GetHash());
                    totalopen += loopData.amount;
                }
                else {
                    closed.push_back(issuancetx.GetHash());
                    totalclosed += loopData.amount;
                }
            }
        });
    
    result.push_back(Pair("n", static_cast<int64_t>(issuances.size() + closed.size())));
    result.push_back(Pair("numpending", static_cast<int64_t>(issuances.size())));
    for (int32_t i = 0; i < issuances.size(); i++)
        a.push_back(issuances[i].GetHex());
    result.push_back(Pair("issuances", a));
    result.push_back(Pair("totalamount", ValueFromAmount(totalopen)));
    result.push_back(Pair("numclosed", static_cast<int64_t>(closed.size())));
    for (int32_t i = 0; i < closed.size(); i++)
        b.push_back(closed[i].GetHex());
    result.push_back(Pair("closed", b));
    result.push_back(Pair("totalclosed", ValueFromAmount(totalclosed)));
    
    return(result);
}

CAmount MarmaraGetLCLAmount() {

    CPubKey refpk = MarmaraGetMyPubkey();
    if (refpk.IsValid())
    {
        // calc lock-in-loops amount for refpk:
        CAmount loopAmount = 0;
        CAmount totalLoopAmount = 0;

        EnumLockedInLoop(
            [&](char *loopaddr, const CTransaction & tx, int32_t nvout, CBlockIndex *pindex) // call enumerator with callback
            {
                totalLoopAmount += tx.vout[nvout].nValue;
            },
            refpk
        );

        return totalLoopAmount;
    }
    return 0;
}


// generate a new activated address and return its segid
UniValue MarmaraNewActivatedAddress(CPubKey pk)
{
    UniValue ret(UniValue::VOBJ);
    char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
    struct CCcontract_info *cp, C;

    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey marmarapk = GetUnspendable(cp, 0);
    
    GetCCaddress1of2(cp, activated1of2addr, marmarapk, pk);
    CKeyID keyID = pk.GetID();
    std::string addr = EncodeDestination(keyID);

    ret.push_back(Pair("pubkey", HexStr(pk.begin(), pk.end())));
    ret.push_back(Pair("normaladdress", addr));
    ret.push_back(Pair("activated1of2address", activated1of2addr));
    ret.push_back(Pair("segid", (int32_t)(komodo_segid32(activated1of2addr) & 0x3f)));
    return ret;
}

//void OS_randombytes(unsigned char *x, long xlen);

// generate 64 activated addresses and split utxos on them
std::string MarmaraLock64(CWallet *pwalletMain, CAmount amount, int32_t nutxos)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
    const CAmount txfee = 10000;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey marmarapk = GetUnspendable(cp, 0);
    CPubKey mypk = pubkey2pk(Mypubkey());

    int32_t height = komodo_nextheight();
    // as opret creation function MarmaraCoinbaseOpret creates opret only for even blocks - adjust this base height to even value
    if ((height & 1) != 0)
        height++;

    // TODO: check that the wallet has already segid pubkeys    
    vACTIVATED_WALLET_DATA activated;
    EnumWalletActivatedAddresses(pwalletMain, activated);
    if (activated.size() >= 64)
    {
        CCerror = "wallet already has 64 activated split addresses. Use a clean wallet with enough normal inputs in it";
        return std::string();
    }

    std::map<uint32_t, std::pair<CKey, CPubKey>> segidKeys;

    // add mypubkey
    char myactivated1of2addr[KOMODO_ADDRESS_BUFSIZE];
    GetCCaddress1of2(cp, myactivated1of2addr, marmarapk, mypk);
    uint32_t segid = komodo_segid32(myactivated1of2addr) & 0x3f;
    if (segidKeys.find(segid) == segidKeys.end())
    {
        // add myprivkey key
        uint8_t mypriv32[32];
        Myprivkey(mypriv32);
        CKey mykey;
        mykey.Set(&mypriv32[0], &mypriv32[32], true);
        segidKeys[segid] = std::make_pair(mykey, mypk);
    }

    while (segidKeys.size() < 64)  // until we do not generate keys for all 64 segids
    {
        uint8_t priv32[32];
        // generate random priv key
#ifndef __WIN32
        OS_randombytes(priv32, sizeof(priv32));
#else
        randombytes_buf(priv32, sizeof(priv32));
#endif
        CKey key;
        key.Set(&priv32[0], &priv32[32], true);
        CPubKey pubkey = key.GetPubKey();
        CKeyID vchAddress = pubkey.GetID();

        // get 1of2 address segid
        char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
        GetCCaddress1of2(cp, activated1of2addr, marmarapk, pubkey);
        uint32_t segid = komodo_segid32(activated1of2addr) & 0x3f;
        if (segidKeys.find(segid) == segidKeys.end())
        {
            // add segid's keys
            segidKeys[segid] = std::make_pair(key, pubkey);
        }
    }

    //std::cerr << "amount / 64LL / (CAmount)nutxos=" << (amount / 64LL / (CAmount)nutxos) << " 100LL * txfee=" << 100LL * txfee << std::endl;

    if (AddNormalinputs(mtx, mypk, amount + txfee + MARMARA_ACTIVATED_MARKER_AMOUNT * 64 * nutxos, CC_MAXVINS) > 0)
    {
        // create tx with 64 * nutxo vouts:
        for (const auto &keyPair : segidKeys)
        {
            for (int32_t i = 0; i < nutxos; i++)
            {
                if (amount / 64LL / (CAmount)nutxos < 100LL * txfee)
                {
                    CCerror = "amount too low";
                    return std::string();
                }
                // lock the amount on 1of2 address:
                CPubKey segidpk = keyPair.second.second;

                // add ccopret
                CScript opret = MarmaraEncodeCoinbaseOpret(/*MARMARA_ACTIVATED*/MARMARA_ACTIVATED_INITIAL, segidpk, height);
                // add marmara opret segpk to each cc vout 
                mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(amount / 64 / nutxos, segidpk, opret));
            }
        }
        mtx.vout.push_back(MakeCC1vout(EVAL_MARMARA, MARMARA_ACTIVATED_MARKER_AMOUNT, marmarapk));
        std::string hextx = FinalizeCCTx(0, cp, mtx, mypk, txfee, CScript(), false);
        if (hextx.empty())
        {
            CCerror = "could not finalize tx";
            return std::string();
        }

        // if tx okay save keys:
        pwalletMain->MarkDirty();
        std::string strLabel = "";
        for (const auto &keyPair : segidKeys)
        {
            CKey key = keyPair.second.first;
            CPubKey pubkey = keyPair.second.second;
            CKeyID vchAddress = pubkey.GetID();

            pwalletMain->SetAddressBook(vchAddress, strLabel, "receive");

            // Don't throw error in case a key is already there
            if (pwalletMain->HaveKey(vchAddress)) {
                LOGSTREAMFN("marmara", CCLOG_INFO, stream << "key already in the wallet" << std::endl);
            }
            else {
                pwalletMain->mapKeyMetadata[vchAddress].nCreateTime = 1;
                if (!pwalletMain->AddKeyPubKey(key, pubkey))
                {
                    CCerror = "Error adding key to wallet";
                    return std::string();
                }
                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "key added to wallet addr=" << EncodeDestination(vchAddress) << std::endl);
            }
        }

        // whenever a key is imported, we need to scan the whole chain
        pwalletMain->nTimeFirstKey = 1; // 0 would be considered 'no value'
        return hextx;

    }
    else {
        CCerror = "not enough normal inputs or too many input utxos";
        return std::string();
    }
}

// list activated addresses in the wallet
UniValue MarmaraListActivatedAddresses(CWallet *pwalletMain)
{
    UniValue ret(UniValue::VOBJ);
    UniValue retarray(UniValue::VARR);

    vACTIVATED_WALLET_DATA activated;
    EnumWalletActivatedAddresses(pwalletMain, activated);
    for (const auto &a : activated)
    {
        UniValue elem(UniValue::VOBJ);
        std::string sActivated1of2addr = ACTIVATED_WALLET_DATA_ADDR(a);
        uint32_t segid = ACTIVATED_WALLET_DATA_SEGID(a);
        CAmount amount = ACTIVATED_WALLET_DATA_AMOUNT(a);

        elem.push_back(std::make_pair("activatedaddress", sActivated1of2addr));
        elem.push_back(std::make_pair("segid", (int32_t)segid));
        elem.push_back(std::make_pair("amount", ValueFromAmount(amount)));
        retarray.push_back(elem);
    }
    ret.push_back(Pair("WalletActivatedAddresses", retarray));
    return ret;
}

// release activated coins from 64 segids to normal address
std::string MarmaraReleaseActivatedCoins(CWallet *pwalletMain, const std::string &destaddr)
{
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    const CAmount txfee = 10000;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey mypk = pubkey2pk(Mypubkey());
    CPubKey marmarapk = GetUnspendable(cp, NULL);

    vACTIVATED_WALLET_DATA activated;
    EnumWalletActivatedAddresses(pwalletMain, activated);
    if (activated.size() == 0)
    {
        CCerror = "no activated coins in the wallet (size==0)";
        return std::string();
    }

    int32_t maxvins = 128;

    if (AddNormalinputs(mtx, mypk, txfee, MARMARA_VINS) > 0)
    {
        CAmount total = 0LL;
        for (const auto &a : activated)
        {
            char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
            CKey key = ACTIVATED_WALLET_DATA_KEY(a);
            CPubKey pk = ACTIVATED_WALLET_DATA_PK(a);

            // skip mypubkey
            if (pk != mypk)
            {
                GetCCaddress1of2(cp, activated1of2addr, marmarapk, pk);

                CC *probeCond = MakeCCcond1of2(EVAL_MARMARA, marmarapk, pk);  //add probe condition
                CCAddVintxCond(cp, probeCond, key.begin());
                cc_free(probeCond);

                std::vector<CPubKey> pubkeys;
                CAmount amount = AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, 0, maxvins - mtx.vin.size());  // if total == 0 AddMarmaraCCInputs just calcs but does not adds vins
                if (amount > 0)
                {
                    amount = AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, amount, maxvins - mtx.vin.size());
                    total += amount;
                }
            }
        }

        if (total == 0)
        {
            CCerror = "no activated coins in the wallet (total==0)";
            return std::string();
        }
        CTxDestination dest = DecodeDestination(destaddr.c_str());
        mtx.vout.push_back(CTxOut(total, GetScriptForDestination(dest)));  // where to send activated coins from normal 

        int32_t height = komodo_nextheight();
        // as opret creation function MarmaraCoinbaseOpret creates opret only for even blocks - adjust this base height to even value
        if ((height & 1) != 0)
            height++;
        CScript opret = MarmaraEncodeCoinbaseOpret(MARMARA_RELEASE, mypk, height); // dummy opret

        std::string hextx = FinalizeCCTx(0, cp, mtx, mypk, txfee, opret, false);
        if (hextx.empty())
        {
            CCerror = "could not finalize tx";
            return std::string();
        }
        else
            return hextx;
    }
    else
    {
        CCerror = "insufficient normals for tx fee";
        return std::string();
    }
}


// unlock activated coins from mypk to normal address
std::string MarmaraUnlockActivatedCoins(CAmount amount)
{
    if (!MarmaraIs2020JuneUpdateActive(NULL))   {
        CCerror = "unlocking not available yet";
        return std::string();
    }

    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(Params().GetConsensus(), komodo_nextheight());
    const CAmount txfee = 10000;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    CPubKey mypk = pubkey2pk(Mypubkey());
    CPubKey marmarapk = GetUnspendable(cp, NULL);

    if (AddNormalinputs(mtx, mypk, txfee, MARMARA_VINS) > 0)
    {
        char activated1of2addr[KOMODO_ADDRESS_BUFSIZE];
        CMarmaraActivatedOpretChecker activatedChecker;
        GetCCaddress1of2(cp, activated1of2addr, marmarapk, mypk);

        CC *probeCond = MakeCCcond1of2(EVAL_MARMARA, marmarapk, mypk);  //add probe condition
        CCAddVintxCond(cp, probeCond, NULL);

        std::vector<CPubKey> pubkeys;
        CAmount inputs = AddMarmaraCCInputs(IsMarmaraActivatedVout, mtx, pubkeys, activated1of2addr, amount, MARMARA_VINS);
        if (inputs < amount) {
            CCerror = "insufficient activated coins";
            return std::string();
        }

        mtx.vout.push_back(CTxOut(amount, CScript() << vuint8_t(mypk.begin(), mypk.end()) << OP_CHECKSIG));  // where to send activated coins from normal 
        LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "added amount=" << amount << std::endl);

        int32_t height = komodo_nextheight();
        if ((height & 1) != 0)
            height++;   // make height even as only even height is considered for staking (TODO: strange)

        CAmount change = inputs - amount;
        if (change > 0)
        {
            CScript opret = MarmaraEncodeCoinbaseOpret(MARMARA_ACTIVATED, mypk, height);
            // add coinbase opret to ccvout for the change
            mtx.vout.push_back(MakeMarmaraCC1of2voutOpret(change, mypk, opret));  // adding MarmaraCoinbase cc vout 'opret' for change
        }        
        CScript opret = MarmaraEncodeReleaseOpret(); // dummy opret with release funcid
        std::string hextx = FinalizeCCTx(0, cp, mtx, mypk, txfee, opret, false);
        cc_free(probeCond);
        if (hextx.empty())
        {
            CCerror = "could not finalize tx";
            return std::string();
        }
        else
            return hextx;
    }
    else
    {
        CCerror = "insufficient normals for tx fee";
        return std::string();
    }
}

UniValue MarmaraReceiveList(const CPubKey &pk, int32_t maxage)
{
    UniValue result(UniValue::VARR);
    char coinaddr[KOMODO_ADDRESS_BUFSIZE];
    int64_t nValue, totalinputs = 0;
    uint256 txid, hashBlock;
    CTransaction tx;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;

    struct CCcontract_info *cp, C;
    cp = CCinit(&C, EVAL_MARMARA);
    GetCCaddress(cp, coinaddr, pk);
    SetCCunspents(unspentOutputs, coinaddr, true);

    LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " check coinaddr=" << coinaddr << std::endl);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it = unspentOutputs.begin(); it != unspentOutputs.end(); it++)
    {
        txid = it->first.txhash;
        if (get_next_height() - it->second.blockHeight > maxage)  // skip too old request txns
            continue;

        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " txid=" << txid.GetHex() << std::endl);
        if (myGetTransaction(txid, tx, hashBlock) && !hashBlock.IsNull())
        {
            LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " got txid=" << txid.GetHex() << std::endl);
            if (!tx.IsCoinBase() && tx.vout.size() > 1 && (tx.vout[0].nValue == MARMARA_CREATETX_AMOUNT || tx.vout[0].nValue == MARMARA_BATON_AMOUNT))
            {
                SMarmaraCreditLoopOpret loopData;
                uint8_t funcid = MarmaraDecodeLoopOpret(tx.vout.back().scriptPubKey, loopData, MARMARA_OPRET_VERSION_ANY);
                LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " MarmaraDecodeLoopOpret funcid=" << (int)funcid << std::endl);
                if (funcid == MARMARA_REQUEST)  {
                    get_loop_creation_data(loopData.createtxid, loopData, 0); // update with loop creation data
                }

                if (funcid == MARMARA_CREATELOOP || funcid == MARMARA_REQUEST)    
                {
                    if (loopData.matures > chainActive.LastTip()->GetHeight())   // add request txns only for active loops
                    {
                        LOGSTREAMFN("marmara", CCLOG_DEBUG2, stream << " adding txid=" << txid.GetHex() << std::endl);
                        UniValue info(UniValue::VOBJ);
                        info.push_back(Pair("version", (int)loopData.version));
                        info.push_back(Pair("txid", txid.GetHex()));
                        info.push_back(Pair("creationtxid", loopData.createtxid.GetHex()));
                        info.push_back(Pair("funcid", std::string(1, funcid)));
                        info.push_back(Pair("amount", ValueFromAmount(loopData.amount)));
                        info.push_back(Pair("matures", loopData.matures));
                        
                        // get first normal input pubkey to get who is the receiver:
                        CPubKey receiverpk = GetFirstNormalInputPubKey(tx);
                        info.push_back(Pair("receivepk", HexStr(receiverpk)));
                        info.push_back(Pair("issuerpk", HexStr(loopData.pk)));

                        result.push_back(info);
                    }
                }
            }
        }
    }
    return result;
}


// collects PoS statistics
#define POSSTAT_STAKETXADDR 0
#define POSSTAT_STAKETXTYPE 1
#define POSSTAT_SEGID 2
#define POSSTAT_COINBASEAMOUNT 3
#define POSSTAT_TXCOUNT 4

UniValue MarmaraPoSStat(int32_t beginHeight, int32_t endHeight)
{
    UniValue result(UniValue::VOBJ);
    UniValue array(UniValue::VARR);
    UniValue error(UniValue::VOBJ);

    // old stat:
    /* tuple params:  is boosted, coinbase normal addr, normal total, coinbase cc addr, cc total, txcount, segid */
    // typedef std::tuple<bool, std::string, int64_t, std::string, int64_t, int32_t, uint32_t> TStatElem;

    /* tuple params:  is boosted, coinbase normal addr, normal total, coinbase cc addr, cc total, txcount, segid */
    typedef std::tuple<std::string, std::string, uint32_t, CAmount, int32_t> TStatElem;
    std::map<std::string, TStatElem> mapStat;

    if (beginHeight == 0)
        beginHeight = 1;
    if (endHeight == 0)
        endHeight = chainActive.Height();

    for(int32_t h = beginHeight; h <= endHeight; h ++) 
    {
        int8_t hsegid = komodo_segid(0, h);

        CBlockIndex *pblockindex = chainActive[h];
        CBlock block;

        if (fHavePruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0) {
            error.push_back(Pair("result", "error"));
            error.push_back(Pair("error", std::string("Block not available (pruned data), h=") + std::to_string(h)));
            return error;
        }

        if (!ReadBlockFromDisk(block, pblockindex, 1)) {
            error.push_back(Pair("result", "error"));
            error.push_back(Pair("error", std::string("Can't read block from disk, h=") + std::to_string(h)));
            return error;
        }
        if (hsegid >= 0)
        {
            if (block.vtx.size() >= 2)
            {
                CTransaction coinbase = block.vtx[0];
                CTransaction stakeTx = block.vtx.back(), vintx;
                uint256 hashBlock;
                vscript_t vopret;

                char staketxaddr[KOMODO_ADDRESS_BUFSIZE];
                Getscriptaddress(staketxaddr, stakeTx.vout[0].scriptPubKey);

                std::string sStakeTxAddr = staketxaddr;
                std::string staketxtype;

                if (stakeTx.vout[0].scriptPubKey.IsPayToCryptoCondition()) 
                {
                    CMarmaraActivatedOpretChecker activatedChecker;
                    CMarmaraLockInLoopOpretChecker lclChecker(CHECK_ONLY_CCOPRET, MARMARA_OPRET_VERSION_ANY);
                    CScript opret;
                    CPubKey opretpk;
                    vscript_t vopret;

                    if (get_either_opret(&activatedChecker, stakeTx, 0, opret, opretpk) && GetOpReturnData(opret, vopret) && vopret.size() >= 2)
                    {
                        if (IsFuncidOneOf(vopret[1], MARMARA_ACTIVATED_1X_FUNCIDS))
                        {
                            staketxtype = "activated-1x";
                        }
                        else if (IsFuncidOneOf(vopret[1], MARMARA_ACTIVATED_3X_FUNCIDS))
                        {
                            staketxtype = "activated-3x";
                        }
                        else
                        {
                            staketxtype = "activated-unknown";
                        }
                    }
                    else if (get_either_opret(&lclChecker, stakeTx, 0, opret, opretpk) && GetOpReturnData(opret, vopret) && vopret.size() >= 2)
                    {
                        staketxtype = "boosted";
                    }
                    else
                    {
                        LOGSTREAMFN("marmara", CCLOG_ERROR, stream << "could not get stake tx opret txid=" << stakeTx.GetHash().GetHex() << " h=" << h << std::endl);
                        error.push_back(Pair("result", "error"));
                        error.push_back(Pair("error", std::string("Stake transaction opret not recognized, h=") + std::to_string(h)));
                        return error;
                    }
                }
                else
                {
                    staketxtype = "normal";  // normal stake tx not supported in marmara, only activated or lcl
                }

                TStatElem elem = mapStat[sStakeTxAddr + staketxtype];

                CAmount amount = std::get<POSSTAT_COINBASEAMOUNT>(elem) + coinbase.vout[0].nValue;
                uint32_t segid = komodo_segid32(staketxaddr) & 0x3f;
                mapStat[sStakeTxAddr + staketxtype] = std::make_tuple(sStakeTxAddr, staketxtype, segid, amount, std::get<POSSTAT_TXCOUNT>(elem) + 1);

                LOGSTREAMFN("marmara", CCLOG_DEBUG1, stream << "h=" << h << " stake-txid=" << stakeTx.GetHash().GetHex() << " segid=" << segid << " address=" << staketxaddr << " type=" << staketxtype << " amount=" << stakeTx.vout[0].nValue << std::endl);
            }
        }
        else {
            CTransaction coinbase = block.vtx[0];
            char cbaddr[KOMODO_ADDRESS_BUFSIZE];
            Getscriptaddress(cbaddr, coinbase.vout[0].scriptPubKey);
            TStatElem elem = mapStat[std::string(cbaddr) + "pow"];

            CAmount amount = std::get<POSSTAT_COINBASEAMOUNT>(elem) + coinbase.vout[0].nValue;
            mapStat[std::string(cbaddr) + "pow"] = std::make_tuple(std::string(cbaddr), "pow", 0, amount, std::get<POSSTAT_TXCOUNT>(elem) + 1);    
        }
    }

    for (const auto &eStat : mapStat)
    {
        UniValue elem(UniValue::VOBJ);

        elem.push_back(Pair("CoinbaseAddress", std::get<POSSTAT_STAKETXADDR>(eStat.second)));
        elem.push_back(Pair("BlockType", std::get<POSSTAT_STAKETXTYPE>(eStat.second)));
        elem.push_back(Pair("segid", (uint64_t)std::get<POSSTAT_SEGID>(eStat.second)));
        elem.push_back(Pair("CoinbaseAmount", std::get<POSSTAT_COINBASEAMOUNT>(eStat.second)));
        elem.push_back(Pair("TxCount", std::get<POSSTAT_TXCOUNT>(eStat.second)));
        array.push_back(elem);
    }

    result.push_back(Pair("result", "success"));
    result.push_back(Pair("BeginHeight", beginHeight));
    result.push_back(Pair("EndHeight", endHeight));
    result.push_back(Pair("StakingStat", array));
    return result;
}

// utils
static void decode_marmara_opret_to_univalue(const CScript &opret, UniValue &univout)
{
    SMarmaraCreditLoopOpret loopData;
    uint8_t ver, funcid;
    int32_t h, uh, matureht;
    CPubKey pk;
    vuint8_t vopret;

    GetOpReturnData(opret, vopret);
    if (vopret.size() > 0) {
        uint8_t evalcode = vopret.begin()[0];
        char seval[8];
        sprintf(seval, "0x%02X", (int)evalcode);
        univout.push_back(Pair("eval", seval));
    }
    if (MarmaraDecodeLoopOpret(opret, loopData, MARMARA_OPRET_VERSION_ANY) != 0)
    {
        univout.push_back(Pair("funcid", std::string(1, loopData.lastfuncid)));
        if (loopData.lastfuncid == MARMARA_CREATELOOP)
            univout.push_back(Pair("description", "create-loop"));
        else if (loopData.lastfuncid == MARMARA_REQUEST)
            univout.push_back(Pair("description", "request"));
        else if (loopData.lastfuncid == MARMARA_ISSUE)
            univout.push_back(Pair("description", "issue"));
        else if (loopData.lastfuncid == MARMARA_TRANSFER)
            univout.push_back(Pair("description", "transfer"));
        else if (loopData.lastfuncid == MARMARA_LOCKED)
            univout.push_back(Pair("description", "locked-in-loop"));
        else if (loopData.lastfuncid == MARMARA_SETTLE)
            univout.push_back(Pair("description", "settlement"));
        else if (loopData.lastfuncid == MARMARA_SETTLE_PARTIAL)
            univout.push_back(Pair("description", "settlement-partial"));

        if (loopData.lastfuncid == MARMARA_CREATELOOP) {
            univout.push_back(Pair("sender-pubkey", HexStr(loopData.pk.begin(), loopData.pk.end())));
            univout.push_back(Pair("loop-amount", loopData.amount));
            univout.push_back(Pair("mature-height", (int64_t)loopData.matures));
            univout.push_back(Pair("currency", loopData.currency));
        } 
        if (loopData.lastfuncid == MARMARA_REQUEST) {
            univout.push_back(Pair("sender-pubkey", HexStr(loopData.pk.begin(), loopData.pk.end())));
            univout.push_back(Pair("loop-create-txid", loopData.createtxid.GetHex()));
        } 
        if (loopData.lastfuncid == MARMARA_ISSUE || loopData.lastfuncid == MARMARA_TRANSFER) {
            univout.push_back(Pair("receiver-pubkey", HexStr(loopData.pk.begin(), loopData.pk.end())));
            univout.push_back(Pair("loop-create-txid", loopData.createtxid.GetHex()));
        }
        else if (loopData.lastfuncid == MARMARA_LOCKED) {
            univout.push_back(Pair("endorser-pubkey", HexStr(loopData.pk.begin(), loopData.pk.end())));
            univout.push_back(Pair("loop-create-txid", loopData.createtxid.GetHex()));
        }
        else if (loopData.lastfuncid == MARMARA_SETTLE || loopData.lastfuncid == MARMARA_SETTLE_PARTIAL) {
            univout.push_back(Pair("holder-pubkey", HexStr(loopData.pk.begin(), loopData.pk.end())));
            univout.push_back(Pair("loop-create-txid", loopData.createtxid.GetHex()));
        }
        else {
            univout.push_back(Pair("error", "unknown funcid"));
        }

        if (!loopData.createtxid.IsNull())
        {
            CPubKey createtxidPk = CCtxidaddr_tweak(NULL, loopData.createtxid);
            char ccaddr[KOMODO_ADDRESS_BUFSIZE];
            CTxOut tvout = MakeMarmaraCC1of2voutOpret(loopData.amount, createtxidPk, CScript());
            Getscriptaddress(ccaddr, tvout.scriptPubKey);
            univout.push_back(Pair("loop-create-txid-1of2-addr", ccaddr));
        }
    }
    else if ((funcid = MarmaraDecodeCoinbaseOpretExt(opret, ver, pk, h, uh, matureht)) != 0)
    {
        univout.push_back(Pair("version", (int)ver));
        univout.push_back(Pair("funcid", std::string(1, funcid)));
        if (funcid == MARMARA_ACTIVATED)
            univout.push_back(Pair("description", "activated-1x"));
        else if (funcid == MARMARA_COINBASE)
            univout.push_back(Pair("description", "coinbase-1x"));
        else if (funcid == MARMARA_COINBASE_3X)
            univout.push_back(Pair("description", "coinbase-3x"));
        else if (funcid == MARMARA_ACTIVATED_INITIAL)
            univout.push_back(Pair("description", "activated_lock64"));
        else if (funcid == MARMARA_POOL)
            univout.push_back(Pair("description", "pool"));
        if (pk.IsValid()) {
            univout.push_back(Pair("pubkey", HexStr(pk.begin(), pk.end())));
            char ccaddr[KOMODO_ADDRESS_BUFSIZE];
            CTxOut tvout = MakeMarmaraCC1of2voutOpret(loopData.amount, pk, CScript());
            Getscriptaddress(ccaddr, tvout.scriptPubKey);
            univout.push_back(Pair("pubkey-1of2-addr", ccaddr));
        }
        if (ver == 2)
            univout.push_back(Pair("matureHeight", matureht));
    }
    else if ((funcid = MarmaraDecodeReleaseOpret(opret, ver, MARMARA_OPRET_VERSION_ANY)) != 0)
    {
        univout.push_back(Pair("version", (int)ver));
        univout.push_back(Pair("funcid", std::string(1, funcid)));
        if (funcid == MARMARA_RELEASE)
            univout.push_back(Pair("description", "release"));
    }
}

void decode_marmara_vout(const CTxOut &vout, UniValue &univout)
{
    vuint8_t vopret;

    if (!GetOpReturnData(vout.scriptPubKey, vopret))
    {
        char addr[KOMODO_ADDRESS_BUFSIZE];

        univout.push_back(Pair("nValue", vout.nValue));
        Getscriptaddress(addr, vout.scriptPubKey);
        univout.push_back(Pair("address", addr));

        if (vout.scriptPubKey.IsPayToCryptoCondition())
        {
            CScript ccopret;

            univout.push_back(Pair("vout-type", "cryptocondition"));
            if (MyGetCCopret(vout.scriptPubKey, ccopret))
            {
                decode_marmara_opret_to_univalue(ccopret, univout);
            }
            else
            {
                univout.push_back(Pair("ccdata", "no"));
            }
        }
        else
        {
            univout.push_back(Pair("vout-type", "normal"));
        }
    }
    else
    {
        univout.push_back(Pair("vout-type", "opreturn"));
        decode_marmara_opret_to_univalue(vout.scriptPubKey, univout);
    }
}

UniValue MarmaraDecodeTxdata(const vuint8_t &txdata, bool printvins)
{
    UniValue result(UniValue::VOBJ);
    CTransaction tx;
    vuint8_t vopret;

    if (E_UNMARSHAL(txdata, ss >> tx))
    {
        result.push_back(Pair("object", "transaction"));

        if (printvins)
        {
            UniValue univins(UniValue::VARR);

            if (tx.IsCoinBase())
            {
                UniValue univin(UniValue::VOBJ);
                univin.push_back(Pair("coinbase", ""));
                univins.push_back(univin);
            }
            else if (tx.IsCoinImport())
            {
                UniValue univin(UniValue::VOBJ);
                univin.push_back(Pair("coinimport", ""));
                univins.push_back(univin);
            }
            else
            {
                for (int i = 0; i < tx.vin.size(); i++)
                {
                    CTransaction vintx;
                    uint256 hashBlock;
                    UniValue univin(UniValue::VOBJ);

                    univin.push_back(Pair("n", std::to_string(i)));
                    univin.push_back(Pair("prev-txid", tx.vin[i].prevout.hash.GetHex()));
                    univin.push_back(Pair("prev-n", (int64_t)tx.vin[i].prevout.n));
                    if (myGetTransaction(tx.vin[i].prevout.hash, vintx, hashBlock))
                    {
                        UniValue univintx(UniValue::VOBJ);
                        decode_marmara_vout(vintx.vout[tx.vin[i].prevout.n], univintx);
                        univin.push_back(Pair("vout", univintx));
                    }
                    else
                    {
                        univin.push_back(Pair("error", "could not load vin tx"));
                    }
                    univins.push_back(univin);
                }
            }
            result.push_back(Pair("vins", univins));
        }

        UniValue univouts(UniValue::VARR);

        for (int i = 0; i < tx.vout.size(); i ++)
        {
            UniValue univout(UniValue::VOBJ);

            univout.push_back(Pair("n", std::to_string(i)));
            decode_marmara_vout(tx.vout[i], univout);            
            univouts.push_back(univout);
        }
        result.push_back(Pair("vouts", univouts));
    }
    else 
    {
        CScript opret(txdata.begin(), txdata.end());
        CScript ccopret;
        UniValue univout(UniValue::VOBJ);

        if (GetOpReturnData(opret, vopret))
        {
            univout.push_back(Pair("object", "opreturn"));
            decode_marmara_opret_to_univalue(opret, univout);
            result.push_back(Pair("decoded", univout));
        }
        else if (MyGetCCopret(opret, ccopret))
        {
            univout.push_back(Pair("object", "vout-ccdata"));
            decode_marmara_opret_to_univalue(ccopret, univout);
            result.push_back(Pair("decoded", univout));
        }
        else {
            result.push_back(Pair("object", "cannot decode"));
        }
    }

    return result;
}


// fixes:
static bool skipBadLoop(const uint256 &refbatontxid)
{
    return Parseuint256("a8774a147f5153d8da4c554a4953de06b3b864f681a460cb9e3968a01d144370") == refbatontxid ||
        Parseuint256("8a7fb07112fa8e99f3480485921df2119097e4ea34cb5c59449f34fdac74e266") == refbatontxid ||
        Parseuint256("7d20cc53b11488600e61d349c16e5e2f9cdd905ad86aca8c4bfdf7dd0f6b6242") == refbatontxid ||
        Parseuint256("01208c5b322d444cdcc07f09bfaef8e6cca7f65c6c580d1cf6cde6b063dee98d") == refbatontxid;
}

static bool fixBadSettle(const uint256 &settletxid)
{
    return Parseuint256("57ae9f4a36ece775041ede5f0792831861428552f16eaf44cff9001020542d05") == settletxid && get_next_height() < MARMARA_POS_IMPROVEMENTS_HEIGHT;
}

/* old version of amount stat
UniValue MarmaraAmountStatSnapshot(int32_t beginHeight, int32_t endHeight)
{
    UniValue result(UniValue::VOBJ);
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;

    if (beginHeight == 0)
        beginHeight = 1;
    if (endHeight == 0)
        endHeight = chainActive.Height();

    CAmount normals = 0LL;
    CAmount ppsh = 0LL;
    CAmount lcl = 0LL;
    CAmount activated = 0LL;
    CAmount ccunk = 0LL;

    if (!pblocktree->ReadAllUnspentIndex(unspentOutputs))  {
        result.push_back(Pair("result", "error"));
        result.push_back(Pair("error", "could not get snapshot"));
        return result;
    }
    for (auto const &u : unspentOutputs)
    {
        UniValue elem(UniValue::VOBJ);

        if (u.second.blockHeight >= beginHeight && u.second.blockHeight <= endHeight)
        {
            if (u.first.type == 3) // cc
            {
                CTransaction tx;
                uint256 hb, crtxid;

                if (myGetTransaction(u.first.txhash, tx, hb)) {
                    CPubKey pk;
                    //char addr[KOMODO_ADDRESS_BUFSIZE];
                    //Getscriptaddress(addr, tx.vout[u.first.index].scriptPubKey);
                    if (IsMarmaraActivatedVout(tx, u.first.index, pk, crtxid)) {
                        activated += tx.vout[u.first.index].nValue;
                    }
                    else if (IsMarmaraLockedInLoopVout(tx, u.first.index, pk, crtxid)) {
                        lcl += tx.vout[u.first.index].nValue;
                    }
                    else
                    {
                        ccunk += tx.vout[u.first.index].nValue;
                    }
                }
                else {
                    std::cerr << __func__ << " " << "could not read a tx=" << u.first.txhash.GetHex() << std::endl;
                    result.push_back(Pair("result", "error"));
                    result.push_back(Pair("error", "could not get tx"));
                    return result;
                }
            }
            else if (u.first.type == 1) // normal
            {
                //char addr[KOMODO_ADDRESS_BUFSIZE];
                //Getscriptaddress(addr, u.second.script);
                normals += u.second.satoshis;
            }
            else // if (u.first.type == 2) // script
            {
                //char addr[KOMODO_ADDRESS_BUFSIZE];
                //Getscriptaddress(addr, u.second.script);
                ppsh += u.second.satoshis;
            }
        }
    }

    result.push_back(Pair("result", "success"));
    result.push_back(Pair("BeginHeight", beginHeight));
    result.push_back(Pair("EndHeight", endHeight));
    result.push_back(Pair("TotalNormals", ValueFromAmount(normals)));
    result.push_back(Pair("TotalPayToScriptHash", ValueFromAmount(ppsh)));
    result.push_back(Pair("TotalActivated", ValueFromAmount(activated)));
    result.push_back(Pair("TotalLockedInLoops", ValueFromAmount(lcl)));
    result.push_back(Pair("TotalUnknownCC", ValueFromAmount(ccunk)));

    return result;
}
*/

// unspent amounts stat
// returns amounts of unspent utxos within the selected height interval and spent amounts of utxos preceding begin height  
UniValue MarmaraAmountStatDiff(int32_t beginHeight, int32_t endHeight)
{
    UniValue result(UniValue::VOBJ);
    UniValue error(UniValue::VOBJ);

    if (beginHeight == 0)
        beginHeight = 1;
    if (endHeight == 0)
        endHeight = chainActive.Height();

    CAmount normals = 0LL;
    CAmount ppsh = 0LL;
    CAmount lcl = 0LL;
    CAmount activated = 0LL;
    CAmount ccunk = 0LL;

    CAmount spentNormals = 0LL;
    CAmount spentPpsh = 0LL;
    CAmount spentLcl = 0LL;
    CAmount spentActivated = 0LL;
    CAmount spentCcunk = 0LL;

    for(int32_t h = beginHeight; h <= endHeight; h ++) 
    {
        CBlockIndex *pblockindex = chainActive[h];
        CBlock block;

        if (fHavePruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0) {
            error.push_back(Pair("result", "error"));
            error.push_back(Pair("error", std::string("Block not available (pruned data), h=") + std::to_string(h)));
            return error;
        }

        if (!ReadBlockFromDisk(block, pblockindex, 1)) {
            error.push_back(Pair("result", "error"));
            error.push_back(Pair("error", std::string("Can't read block from disk, h=") + std::to_string(h)));
            return error;
        }

        // add totals unspent or spent 
        auto addTotals = [&](const CTransaction &tx, int32_t ivout, bool bSpent) {
            if (tx.vout[ivout].scriptPubKey.IsPayToCryptoCondition()) 
            {
                CPubKey pk;
                uint256 crtxid; 

                if (IsMarmaraActivatedVout(tx, ivout, pk, crtxid)) {
                    if (!bSpent)
                        activated += tx.vout[ivout].nValue;
                    else
                        spentActivated += tx.vout[ivout].nValue;
                }
                else if (IsMarmaraLockedInLoopVout(tx, ivout, pk, crtxid)) {
                    if (!bSpent)
                        lcl += tx.vout[ivout].nValue;
                    else
                        spentLcl += tx.vout[ivout].nValue;
                }
                else {
                    if (!bSpent)
                        ccunk += tx.vout[ivout].nValue;
                    else
                        spentCcunk += tx.vout[ivout].nValue;
                }
            }
            else
            {
                txnouttype whichType;
                std::vector<vuint8_t> vSolutions;
                Solver(tx.vout[ivout].scriptPubKey, whichType, vSolutions);
                if (whichType == TX_SCRIPTHASH) {
                    if (!bSpent)
                        ppsh += tx.vout[ivout].nValue;
                    else
                        spentPpsh += tx.vout[ivout].nValue;
                }
                else {
                    if (!bSpent)
                        normals += tx.vout[ivout].nValue;
                    else
                        spentNormals += tx.vout[ivout].nValue;
                }
            }
        }; 
        
        for (auto const &tx : block.vtx)
        {
            // add utxo unspent within the set block interval
            for (int32_t ivout = 0; ivout < tx.vout.size(); ivout ++)
            {
                uint256 spenttxid;
                int32_t spentvin, spentheight;
                bool bSpent = false;
                if (CCgetspenttxid(spenttxid, spentvin, spentheight, tx.GetHash(), ivout) == 0)
                {
                    CTransaction spenttx;
                    uint256 hashBlock;

                    if (GetTransaction(spenttxid, spenttx, hashBlock))  {
                        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
                        if (mi != mapBlockIndex.end() && (*mi).second) {
                            CBlockIndex* pindex = (*mi).second;
                            if (chainActive.Contains(pindex)) 
                                bSpent = true;
                        }
                    }
                }
                if (!bSpent || spentheight > endHeight || spentheight == 0)
                    addTotals(tx, ivout, false);
            }
                
            // add spent utxos from the previous blocks:
            if (!tx.IsMint())
            {
                for(int32_t ivin = 0; ivin < tx.vin.size(); ivin ++) 
                {   
                    CTransaction vintx;
                    uint256 hashBlock;
                    if (!myGetTransaction(tx.vin[ivin].prevout.hash, vintx, hashBlock)) {
                        error.push_back(Pair("result", "error"));
                        error.push_back(Pair("error", std::string("could not get vin tx=") + tx.vin[ivin].prevout.hash.GetHex()));
                        return error;                        
                    }
                    CBlockIndex *blockIndex = komodo_getblockindex(hashBlock);
                    if (blockIndex == NULL || blockIndex->GetHeight() <= 0) {
                        error.push_back(Pair("result", "error"));
                        error.push_back(Pair("error", std::string("could not get block height for block=") + hashBlock.GetHex()));
                        return error; 
                    }
                    if (blockIndex->GetHeight() < beginHeight) {
                        addTotals(vintx, tx.vin[ivin].prevout.n, true);
                    }
                }
            }
        }                 
    }

    result.push_back(Pair("result", "success"));
    result.push_back(Pair("BeginHeight", beginHeight));
    result.push_back(Pair("EndHeight", endHeight));
    result.push_back(Pair("TotalNormals", ValueFromAmount(normals)));
    result.push_back(Pair("TotalPayToScriptHash", ValueFromAmount(ppsh)));
    result.push_back(Pair("TotalActivated", ValueFromAmount(activated)));
    result.push_back(Pair("TotalLockedInLoops", ValueFromAmount(lcl)));
    result.push_back(Pair("TotalUnknownCC", ValueFromAmount(ccunk)));
    result.push_back(Pair("SpentNormals", ValueFromAmount(spentNormals)));
    result.push_back(Pair("SpentPayToScriptHash", ValueFromAmount(spentPpsh)));
    result.push_back(Pair("SpentActivated", ValueFromAmount(spentActivated)));
    result.push_back(Pair("SpentLockedInLoops", ValueFromAmount(spentLcl)));
    result.push_back(Pair("SpentUnknownCC", ValueFromAmount(spentCcunk)));
    return result;
}