// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The hemis Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "budget/budgetmanager.h"
#include "budget/budgetutil.h"
#include "db.h"
#include "evo/deterministicgms.h"
#include "key_io.h"
#include "gamemaster-payments.h"
#include "gamemaster-sync.h"
#include "gamemasterconfig.h"
#include "gamemasterman.h"
#include "messagesigner.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "rpc/server.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/rpcwallet.h"
#endif

#include <univalue.h>

void budgetToJSON(const CBudgetProposal* pbudgetProposal, UniValue& bObj, int nCurrentHeight)
{
    CTxDestination address1;
    ExtractDestination(pbudgetProposal->GetPayee(), address1);

    bObj.pushKV("Name", pbudgetProposal->GetName());
    bObj.pushKV("URL", pbudgetProposal->GetURL());
    bObj.pushKV("Hash", pbudgetProposal->GetHash().ToString());
    bObj.pushKV("FeeHash", pbudgetProposal->GetFeeTXHash().ToString());
    bObj.pushKV("BlockStart", (int64_t)pbudgetProposal->GetBlockStart());
    bObj.pushKV("BlockEnd", (int64_t)pbudgetProposal->GetBlockEnd());
    bObj.pushKV("TotalPaymentCount", (int64_t)pbudgetProposal->GetTotalPaymentCount());
    bObj.pushKV("RemainingPaymentCount", (int64_t)pbudgetProposal->GetRemainingPaymentCount(nCurrentHeight));
    bObj.pushKV("PaymentAddress", EncodeDestination(address1));
    bObj.pushKV("Ratio", pbudgetProposal->GetRatio());
    bObj.pushKV("Yeas", (int64_t)pbudgetProposal->GetYeas());
    bObj.pushKV("Nays", (int64_t)pbudgetProposal->GetNays());
    bObj.pushKV("Abstains", (int64_t)pbudgetProposal->GetAbstains());
    bObj.pushKV("TotalPayment", ValueFromAmount(pbudgetProposal->GetAmount() * pbudgetProposal->GetTotalPaymentCount()));
    bObj.pushKV("MonthlyPayment", ValueFromAmount(pbudgetProposal->GetAmount()));
    bObj.pushKV("IsEstablished", pbudgetProposal->IsEstablished());
    bool fValid = pbudgetProposal->IsValid();
    bObj.pushKV("IsValid", fValid);
    if (!fValid)
        bObj.pushKV("IsInvalidReason", pbudgetProposal->IsInvalidReason());
    bObj.pushKV("Allotted", ValueFromAmount(pbudgetProposal->GetAllotted()));
}

void checkBudgetInputs(const UniValue& params, std::string &strProposalName, std::string &strURL,
                       int &nPaymentCount, int &nBlockStart, CTxDestination &address, CAmount &nAmount)
{
    strProposalName = SanitizeString(params[0].get_str());
    if (strProposalName.size() > 20)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid proposal name, limit of 20 characters.");

    strURL = SanitizeString(params[1].get_str());
    std::string strErr;
    if (!validateURL(strURL, strErr))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strErr);

    nPaymentCount = params[2].get_int();
    if (nPaymentCount < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid payment count, must be more than zero.");

    int nMaxPayments = Params().GetConsensus().nMaxProposalPayments;
    if (nPaymentCount > nMaxPayments) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                strprintf("Invalid payment count, must be <= %d", nMaxPayments));
    }

    CBlockIndex* pindexPrev = GetChainTip();
    if (!pindexPrev)
        throw JSONRPCError(RPC_IN_WARMUP, "Try again after active chain is loaded");

    // Start must be in the next budget cycle or later
    const int budgetCycleBlocks = Params().GetConsensus().nBudgetCycleBlocks;
    int pHeight = pindexPrev->nHeight;

    int nBlockMin = pHeight - (pHeight % budgetCycleBlocks) + budgetCycleBlocks;

    nBlockStart = params[3].get_int();
    if ((nBlockStart < nBlockMin) || ((nBlockStart % budgetCycleBlocks) != 0))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid block start - must be a budget cycle block. Next valid block: %d", nBlockMin));

    address = DecodeDestination(params[4].get_str());
    if (!IsValidDestination(address))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid hemis address");

    nAmount = AmountFromValue(params[5]);
    if (nAmount < 10 * COIN)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid amount - Payment of %s is less than minimum 10 %s allowed", FormatMoney(nAmount), CURRENCY_UNIT));

    const CAmount& nTotalBudget = g_budgetman.GetTotalBudget(nBlockStart);
    if (nAmount > nTotalBudget)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid amount - Payment of %s more than max of %s", FormatMoney(nAmount), FormatMoney(nTotalBudget)));
}

UniValue preparebudget(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 6)
        throw std::runtime_error(
            "preparebudget \"name\" \"url\" npayments start \"address\" monthly_payment\n"
            "\nPrepare proposal for network by signing and creating tx\n"

            "\nArguments:\n"
            "1. \"name\":        (string, required) Desired proposal name (20 character limit)\n"
            "2. \"url\":         (string, required) URL of proposal details (64 character limit)\n"
            "3. npayments:       (numeric, required) Total number of monthly payments\n"
            "4. start:           (numeric, required) Starting super block height\n"
            "5. \"address\":     (string, required) hemis address to send payments to\n"
            "6. monthly_payment: (numeric, required) Monthly payment amount\n"

            "\nResult:\n"
            "\"xxxx\"       (string) proposal fee hash (if successful) or error message (if failed)\n"

            "\nExamples:\n" +
            HelpExampleCli("preparebudget", "\"test-proposal\" \"https://forum.hemis.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
            HelpExampleRpc("preparebudget", "\"test-proposal\" \"https://forum.hemis.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strProposalName;
    std::string strURL;
    int nPaymentCount;
    int nBlockStart;
    CTxDestination address;
    CAmount nAmount;

    checkBudgetInputs(request.params, strProposalName, strURL, nPaymentCount, nBlockStart, address, nAmount);

    // Parse hemis address
    CScript scriptPubKey = GetScriptForDestination(address);

    // create transaction 15 minutes into the future, to allow for confirmation time
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, UINT256_ZERO);
    const uint256& nHash = proposal.GetHash();
    if (!proposal.IsWellFormed(g_budgetman.GetTotalBudget(proposal.GetBlockStart())))
        throw std::runtime_error("Proposal is not valid " + proposal.IsInvalidReason());

    CTransactionRef wtx;
    // make our change address
    CReserveKey keyChange(pwallet);
    if (!pwallet->CreateBudgetFeeTX(wtx, nHash, keyChange, BUDGET_FEE_TX_OLD)) { // 50 HMS collateral for proposal
        throw std::runtime_error("Error making collateral transaction for proposal. Please check your wallet balance.");
    }

    //send the tx to the network
    const CWallet::CommitResult& res = pwallet->CommitTransaction(wtx, keyChange, g_connman.get());
    if (res.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, res.ToString());

    // Store proposal name as a comment
    auto it = pwallet->mapWallet.find(wtx->GetHash());
    assert(it != pwallet->mapWallet.end());
    it->second.SetComment("Proposal: " + strProposalName);

    return wtx->GetHash().ToString();
}

UniValue submitbudget(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 7)
        throw std::runtime_error(
            "submitbudget \"name\" \"url\" npayments start \"address\" monthly_payment \"fee_txid\"\n"
            "\nSubmit proposal to the network\n"

            "\nArguments:\n"
            "1. \"name\":         (string, required) Desired proposal name (20 character limit)\n"
            "2. \"url\":          (string, required) URL of proposal details (64 character limit)\n"
            "3. npayments:        (numeric, required) Total number of monthly payments\n"
            "4. start:            (numeric, required) Starting super block height\n"
            "5. \"address\":      (string, required) hemis address to send payments to\n"
            "6. monthly_payment:  (numeric, required) Monthly payment amount\n"
            "7. \"fee_txid\":     (string, required) Transaction hash from preparebudget command\n"

            "\nResult:\n"
            "\"xxxx\"       (string) proposal hash (if successful) or error message (if failed)\n"

            "\nExamples:\n" +
            HelpExampleCli("submitbudget", "\"test-proposal\" \"https://forum.hemis.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
            HelpExampleRpc("submitbudget", "\"test-proposal\" \"https://forum.hemis.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

    std::string strProposalName;
    std::string strURL;
    int nPaymentCount;
    int nBlockStart;
    CTxDestination address;
    CAmount nAmount;

    checkBudgetInputs(request.params, strProposalName, strURL, nPaymentCount, nBlockStart, address, nAmount);

    // Parse hemis address
    CScript scriptPubKey = GetScriptForDestination(address);
    const uint256& hash = ParseHashV(request.params[6], "parameter 1");

    if (!g_tiertwo_sync_state.IsBlockchainSynced()) {
        throw std::runtime_error("Must wait for client to sync with gamemaster network. Try again in a minute or so.");
    }

    // create the proposal in case we're the first to make it
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, hash);
    if(!g_budgetman.AddProposal(proposal)) {
        std::string strError = strprintf("invalid budget proposal - %s", proposal.IsInvalidReason());
        throw std::runtime_error(strError);
    }
    proposal.Relay();

    return proposal.GetHash().ToString();
}

static CBudgetVote::VoteDirection parseVote(const std::string& strVote)
{
    if (strVote != "yes" && strVote != "no") throw JSONRPCError(RPC_MISC_ERROR, "You can only vote 'yes' or 'no'");
    CBudgetVote::VoteDirection nVote = CBudgetVote::VOTE_ABSTAIN;
    if (strVote == "yes") nVote = CBudgetVote::VOTE_YES;
    if (strVote == "no") nVote = CBudgetVote::VOTE_NO;
    return nVote;
}

UniValue gmbudgetvote(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();

        // Backwards compatibility with legacy `gmbudget` command
        if (strCommand == "vote") strCommand = "local";
        if (strCommand == "vote-many") strCommand = "many";
        if (strCommand == "vote-alias") strCommand = "alias";
    }

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || (request.params.size() == 3 && (strCommand != "local" && strCommand != "many")) || (request.params.size() == 4 && strCommand != "alias") ||
        request.params.size() > 5 || request.params.size() < 3)
        throw std::runtime_error(
            "gmbudgetvote \"local|many|alias\" \"hash\" \"yes|no\" ( \"alias\" legacy )\n"
            "\nVote on a budget proposal\n"
            "\nAfter V6 enforcement, the deterministic gamemaster system is used by default. Set the \"legacy\" parameter to true to vote with legacy gamemasters."

            "\nArguments:\n"
            "1. \"mode\"      (string, required) The voting mode. 'local' for voting directly from a gamemaster, 'many' for voting with a GM controller and casting the same vote for each GM, 'alias' for voting with a GM controller and casting a vote for a single GM\n"
            "2. \"hash\"      (string, required) The budget proposal hash\n"
            "3. \"votecast\"  (string, required) Your vote. 'yes' to vote for the proposal, 'no' to vote against\n"
            "4. \"alias\"     (string, required for 'alias' mode) The GM alias to cast a vote for (for deterministic gamemasters it's the hash of the proTx transaction).\n"
            "5. \"legacy\"    (boolean, optional, default=false) Use the legacy gamemaster system after deterministic gamemasters enforcement.\n"

            "\nResult:\n"
            "{\n"
            "  \"overall\": \"xxxx\",      (string) The overall status message for the vote cast\n"
            "  \"detail\": [\n"
            "    {\n"
            "      \"node\": \"xxxx\",      (string) 'local' or the GM alias\n"
            "      \"result\": \"xxxx\",    (string) Either 'Success' or 'Failed'\n"
            "      \"error\": \"xxxx\",     (string) Error message, if vote failed\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("gmbudgetvote", "\"alias\" \"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\" \"4f9de28fca1f0574a217c5d3c59cc51125ec671de82a2f80b6ceb69673115041\"") +
            HelpExampleRpc("gmbudgetvote", "\"alias\" \"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\" \"4f9de28fca1f0574a217c5d3c59cc51125ec671de82a2f80b6ceb69673115041\""));

    const uint256& hash = ParseHashV(request.params[1], "parameter 1");
    CBudgetVote::VoteDirection nVote = parseVote(request.params[2].get_str());

    bool fLegacyGM = !deterministicGMManager->IsDIP3Enforced() || (request.params.size() > 4 && request.params[4].get_bool());

    if (strCommand == "local") {
        if (!fLegacyGM) {
            throw JSONRPCError(RPC_MISC_ERROR, _("\"local\" vote is no longer available with DGMs. Use \"alias\" from the wallet with the voting key."));
        }
        return gmLocalBudgetVoteInner(true, hash, false, nVote);
    }

    // DGM require wallet with voting key
    if (!fLegacyGM) {
        if (!EnsureWalletIsAvailable(pwallet, false)) {
            return NullUniValue;
        }
        EnsureWalletIsUnlocked(pwallet);
    }

    bool isAlias = false;
    if (strCommand == "many" || (isAlias = strCommand == "alias")) {
        Optional<std::string> gmAlias = isAlias ? Optional<std::string>(request.params[3].get_str()) : nullopt;
        return gmBudgetVoteInner(pwallet, fLegacyGM, hash, false, nVote, gmAlias);
    }

    return NullUniValue;
}

UniValue getbudgetvotes(const JSONRPCRequest& request)
{
    if (request.params.size() != 1)
        throw std::runtime_error(
            "getbudgetvotes \"name\"\n"
            "\nPrint vote information for a budget proposal\n"

            "\nArguments:\n"
            "1. \"name\":      (string, required) Name of the proposal\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"gmId\": \"xxxx-x\",      (string) Gamemaster's outpoint collateral transaction (hash-n)\n"
            "    \"nHash\": \"xxxx\",       (string) Hash of the vote\n"
            "    \"Vote\": \"YES|NO\",      (string) Vote cast ('YES' or 'NO')\n"
            "    \"nTime\": xxxx,         (numeric) Time in seconds since epoch the vote was cast\n"
            "    \"fValid\": true|false,  (boolean) 'true' if the vote is valid, 'false' otherwise\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetvotes", "\"test-proposal\"") + HelpExampleRpc("getbudgetvotes", "\"test-proposal\""));

    std::string strProposalName = SanitizeString(request.params[0].get_str());
    const CBudgetProposal* pbudgetProposal = g_budgetman.FindProposalByName(strProposalName);
    if (pbudgetProposal == nullptr) throw std::runtime_error("Unknown proposal name");
    UniValue ret(UniValue::VARR);
    for (const auto& it : pbudgetProposal->GetVotes()) {
        ret.push_back(it.second.ToJSON());
    }
    return ret;
}

UniValue getnextsuperblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getnextsuperblock\n"
            "\nPrint the next super block height\n"

            "\nResult:\n"
            "n      (numeric) Block height of the next super block\n"

            "\nExamples:\n" +
            HelpExampleCli("getnextsuperblock", "") + HelpExampleRpc("getnextsuperblock", ""));

    int nChainHeight = WITH_LOCK(cs_main, return chainActive.Height());
    if (nChainHeight < 0) return "unknown";

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nNext = nChainHeight - nChainHeight % nBlocksPerCycle + nBlocksPerCycle;
    return nNext;
}

UniValue getbudgetprojection(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getbudgetprojection\n"
            "\nShow the projection of which proposals will be paid the next cycle\n"
            "Proposal fee tx time need to be +24hrs old from the current time. (Testnet is 5 mins)\n"
            "Net Votes needs to be above Gamemaster Count divided by 10\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"Name\": \"xxxx\",               (string) Proposal Name\n"
            "    \"URL\": \"xxxx\",                (string) Proposal URL\n"
            "    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
            "    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
            "    \"BlockStart\": n,              (numeric) Proposal starting block\n"
            "    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
            "    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
            "    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
            "    \"PaymentAddress\": \"xxxx\",     (string) hemis address of payment\n"
            "    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
            "    \"Yeas\": n,                    (numeric) Number of yea votes\n"
            "    \"Nays\": n,                    (numeric) Number of nay votes\n"
            "    \"Abstains\": n,                (numeric) Number of abstains\n"
            "    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount in HMS\n"
            "    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount in HMS\n"
            "    \"IsEstablished\": true|false,  (boolean) Proposal is considered established, 24 hrs after being submitted to network. (Testnet is 5 mins)\n"
            "    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
            "    \"IsInvalidReason\": \"xxxx\",  (string) Error message, if any\n"
            "    \"Allotted\": xxx.xxx,           (numeric) Amount of HMS allotted in current period\n"
            "    \"TotalBudgetAllotted\": xxx.xxx (numeric) Total HMS allotted\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

    UniValue ret(UniValue::VARR);
    UniValue resultObj(UniValue::VOBJ);
    CAmount nTotalAllotted = 0;

    std::vector<CBudgetProposal> winningProps = g_budgetman.GetBudget();
    for (const CBudgetProposal& p : winningProps) {
        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(&p, bObj, g_budgetman.GetBestHeight());
        nTotalAllotted += p.GetAllotted();
        bObj.pushKV("TotalBudgetAllotted", ValueFromAmount(nTotalAllotted));
        ret.push_back(bObj);
    }

    return ret;
}

UniValue getbudgetinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getbudgetinfo ( \"name\" )\n"
            "\nShow current gamemaster budgets\n"

            "\nArguments:\n"
            "1. \"name\"    (string, optional) Proposal name\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"Name\": \"xxxx\",               (string) Proposal Name\n"
            "    \"URL\": \"xxxx\",                (string) Proposal URL\n"
            "    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
            "    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
            "    \"BlockStart\": n,              (numeric) Proposal starting block\n"
            "    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
            "    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
            "    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
            "    \"PaymentAddress\": \"xxxx\",     (string) hemis address of payment\n"
            "    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
            "    \"Yeas\": n,                    (numeric) Number of yea votes\n"
            "    \"Nays\": n,                    (numeric) Number of nay votes\n"
            "    \"Abstains\": n,                (numeric) Number of abstains\n"
            "    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount in HMS\n"
            "    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount in HMS\n"
            "    \"IsEstablished\": true|false,  (boolean) Proposal is considered established, 24 hrs after being submitted to network. (5 mins for Testnet)\n"
            "    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
            "    \"IsInvalidReason\": \"xxxx\",      (string) Error message, if any\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

    UniValue ret(UniValue::VARR);
    int nCurrentHeight = g_budgetman.GetBestHeight();

    if (request.params.size() == 1) {
        std::string strProposalName = SanitizeString(request.params[0].get_str());
        const CBudgetProposal* pbudgetProposal = g_budgetman.FindProposalByName(strProposalName);
        if (pbudgetProposal == nullptr) throw std::runtime_error("Unknown proposal name");
        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(pbudgetProposal, bObj, nCurrentHeight);
        ret.push_back(bObj);
        return ret;
    }

    std::vector<CBudgetProposal*> winningProps = g_budgetman.GetAllProposalsOrdered();
    for (CBudgetProposal* pbudgetProposal : winningProps) {
        if (!pbudgetProposal->IsValid()) continue;

        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(pbudgetProposal, bObj, nCurrentHeight);
        ret.push_back(bObj);
    }

    return ret;
}

UniValue gmbudgetrawvote(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 6)
        throw std::runtime_error(
            "gmbudgetrawvote \"collat_txid\" collat_vout \"hash\" votecast time \"sig\"\n"
            "\nCompile and relay a proposal vote with provided external signature instead of signing vote internally\n"

            "\nArguments:\n"
            "1. \"collat_txid\"   (string, required) Transaction hash for the gamemaster collateral\n"
            "2. collat_vout       (numeric, required) Output index for the gamemaster collateral\n"
            "3. \"hash\"          (string, required) Budget Proposal hash\n"
            "4. \"votecast\"      (string, required) Your vote. 'yes' to vote for the proposal, 'no' to vote against\n"
            "5. time              (numeric, required) Time since epoch in seconds\n"
            "6. \"sig\"           (string, required) External signature\n"

            "\nResult:\n"
            "\"status\"     (string) Vote status or error message\n"

            "\nExamples:\n" +
            HelpExampleCli("gmbudgetrawvote", "") + HelpExampleRpc("gmbudgetrawvote", ""));

    const uint256& hashGmTx = ParseHashV(request.params[0], "gm tx hash");
    int nGmTxIndex = request.params[1].get_int();
    const CTxIn& vin = CTxIn(hashGmTx, nGmTxIndex);

    const uint256& hashProposal = ParseHashV(request.params[2], "Proposal hash");
    CBudgetVote::VoteDirection nVote = parseVote(request.params[3].get_str());

    int64_t nTime = request.params[4].get_int64();
    std::string strSig = request.params[5].get_str();
    bool fInvalid = false;
    std::vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CGamemaster* pgm = gamemasterman.Find(vin.prevout);
    if (!pgm) {
        return "Failure to find gamemaster in list : " + vin.ToString();
    }

    CBudgetVote vote(vin, hashProposal, nVote);
    vote.SetTime(nTime);
    vote.SetVchSig(vchSig);

    if (!vote.CheckSignature(pgm->pubKeyGamemaster.GetID())) {
        return "Failure to verify signature.";
    }

    CValidationState state;
    if (g_budgetman.ProcessProposalVote(vote, nullptr, state)) {
        return "Voted successfully";
    } else {
        return "Error voting : " + state.GetRejectReason() + ". " + state.GetDebugMessage();
    }
}

UniValue gmfinalbudgetsuggest(const JSONRPCRequest& request)
{
    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
                "gmfinalbudgetsuggest\n"
                "\nTry to submit a budget finalization\n"
                "returns the budget hash if it was broadcasted sucessfully");

    if (!Params().IsRegTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR, "command available only for RegTest network");
    }

    const uint256& budgetHash = g_budgetman.SubmitFinalBudget();
    return (budgetHash.IsNull()) ? NullUniValue : budgetHash.ToString();
}

UniValue createrawgmfinalbudget(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
                "createrawgmfinalbudget\n"
                "\nTry to submit the raw budget finalization\n"
                "returns the budget hash if it was broadcasted sucessfully"
                "\nArguments:\n"
                "1. \"budgetname\"    (string, required) finalization name\n"
                "2. \"blockstart\"    (numeric, required) superblock height\n"
                "3. \"proposals\"     (string, required) A json array of json objects\n"
                "     [\n"
                "       {\n"
                "         \"proposalid\":\"id\",  (string, required) The proposal id\n"
                "         \"payee\":n,         (hex, required) The payee script\n"
                "         \"amount\":n            (numeric, optional) The payee amount\n"
                "       }\n"
                "       ,...\n"
                "     ]\n"
                "4. \"feetxid\"    (string, optional) the transaction fee hash\n"
                ""
                "\nResult:\n"
                "{\n"
                    "\"result\"     (string) Budget suggest broadcast or error\n"
                    "\"id\"         (string) id of the fee tx or the finalized budget\n"
                "}\n"
                ); // future: add examples.

    if (!Params().IsRegTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR, "command available only for RegTest network");
    }

    // future: add inputs error checking..
    std::string budName = request.params[0].get_str();
    int nBlockStart = request.params[1].get_int();
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;
    UniValue budgetVec = request.params[2].get_array();
    for (unsigned int idx = 0; idx < budgetVec.size(); idx++) {
        const UniValue& prop = budgetVec[idx].get_obj();
        uint256 propId = ParseHashO(prop, "proposalid");
        std::vector<unsigned char> scriptData(ParseHexO(prop, "payee"));
        CScript payee = CScript(scriptData.begin(), scriptData.end());
        CAmount amount = AmountFromValue(find_value(prop, "amount"));
        vecTxBudgetPayments.emplace_back(propId, payee, amount);
    }

    Optional<uint256> txFeeId = nullopt;
    if (request.params.size() > 3) {
        txFeeId = ParseHashV(request.params[3], "parameter 4");
    }

    if (!txFeeId) {
        CFinalizedBudget tempBudget(budName, nBlockStart, vecTxBudgetPayments, UINT256_ZERO);
        const uint256& budgetHash = tempBudget.GetHash();

        // create fee tx
        CTransactionRef wtx;
        CReserveKey keyChange(vpwallets[0]);
        if (!vpwallets[0]->CreateBudgetFeeTX(wtx, budgetHash, keyChange, BUDGET_FEE_TX)) {
            throw std::runtime_error("Can't make collateral transaction");
        }
        // Send the tx to the network
        const CWallet::CommitResult& res = vpwallets[0]->CommitTransaction(wtx, keyChange, g_connman.get());
        UniValue ret(UniValue::VOBJ);
        if (res.status == CWallet::CommitStatus::OK) {
            ret.pushKV("result", "tx_fee_sent");
            ret.pushKV("id", wtx->GetHash().ToString());
        } else {
            ret.pushKV("result", "error");
        }
        return ret;
    }

    UniValue ret(UniValue::VOBJ);
    // Collateral tx already exists, see if it's mature enough.
    CFinalizedBudget fb(budName, nBlockStart, vecTxBudgetPayments, *txFeeId);
    if (g_budgetman.AddFinalizedBudget(fb)) {
        fb.Relay();
        ret.pushKV("result", "fin_budget_sent");
        ret.pushKV("id", fb.GetHash().ToString());
    } else {
        // future: add proper error
        ret.pushKV("result", "error");
    }
    return ret;
}

static std::string GetFinalizedBudgetStatus(CFinalizedBudget* fb)
{
    std::string retBadHashes;
    std::string retBadPayeeOrAmount;
    int nBlockStart = fb->GetBlockStart();
    int nBlockEnd = fb->GetBlockEnd();

    for (int nBlockHeight = nBlockStart; nBlockHeight <= nBlockEnd; nBlockHeight++) {
        CTxBudgetPayment budgetPayment;
        if (!fb->GetBudgetPaymentByBlock(nBlockHeight, budgetPayment)) {
            LogPrint(BCLog::GMBUDGET,"%s: Couldn't find budget payment for block %lld\n", __func__, nBlockHeight);
            continue;
        }

        CBudgetProposal bp;
        if (!g_budgetman.GetProposal(budgetPayment.nProposalHash, bp)) {
            retBadHashes += (retBadHashes.empty() ? "" : ", ") + budgetPayment.nProposalHash.ToString();
            continue;
        }

        if (bp.GetPayee() != budgetPayment.payee || bp.GetAmount() != budgetPayment.nAmount) {
            retBadPayeeOrAmount += (retBadPayeeOrAmount.empty() ? "" : ", ") + budgetPayment.nProposalHash.ToString();
        }
    }

    if (retBadHashes.empty() && retBadPayeeOrAmount == "") return "OK";

    if (!retBadHashes.empty()) retBadHashes = "Unknown proposal(s) hash! Check this proposal(s) before voting: " + retBadHashes;
    if (!retBadPayeeOrAmount.empty()) retBadPayeeOrAmount = "Budget payee/nAmount doesn't match our proposal(s)! "+ retBadPayeeOrAmount;

    return retBadHashes + " -- " + retBadPayeeOrAmount;
}

UniValue gmfinalbudget(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp ||
        (strCommand != "vote-many" && strCommand != "vote" && strCommand != "show" && strCommand != "getvotes"))
        throw std::runtime_error(
            "gmfinalbudget \"command\"... ( \"passphrase\" )\n"
            "\nVote or show current budgets\n"

            "\nAvailable commands:\n"
            "  vote-many   - Vote on a finalized budget\n"
            "  vote        - Vote on a finalized budget with local gamemaster\n"
            "  show        - Show existing finalized budgets\n"
            "  getvotes     - Get vote information for each finalized budget\n");

    if (strCommand == "vote-many" || strCommand == "vote") {
        if (request.params.size() < 2 || request.params.size() > 3) {
            throw std::runtime_error(strprintf("Correct usage is 'gmfinalbudget %s BUDGET_HASH (fLegacy)'", strCommand));
        }
        const uint256& hash = ParseHashV(request.params[1], "BUDGET_HASH");
        bool fLegacyGM = !deterministicGMManager->IsDIP3Enforced() || (request.params.size() > 2 && request.params[2].get_bool());

        // DGM require wallet with operator keys for vote-many
        if (!fLegacyGM && strCommand == "vote-many" && !EnsureWalletIsAvailable(pwallet, false)) {
            return NullUniValue;
        }

        return (strCommand == "vote-many" ? gmBudgetVoteInner(pwallet, fLegacyGM, hash, true, CBudgetVote::VOTE_YES, nullopt)
                                          : gmLocalBudgetVoteInner(fLegacyGM, hash, true, CBudgetVote::VOTE_YES));
    }

    if (strCommand == "show") {
        UniValue resultObj(UniValue::VOBJ);

        std::vector<CFinalizedBudget*> winningFbs = g_budgetman.GetFinalizedBudgets();
        for (CFinalizedBudget* finalizedBudget : winningFbs) {
            const uint256& nHash = finalizedBudget->GetHash();
            UniValue bObj(UniValue::VOBJ);
            bObj.pushKV("FeeTX", finalizedBudget->GetFeeTXHash().ToString());
            bObj.pushKV("BlockStart", (int64_t)finalizedBudget->GetBlockStart());
            bObj.pushKV("BlockEnd", (int64_t)finalizedBudget->GetBlockEnd());
            bObj.pushKV("Proposals", finalizedBudget->GetProposalsStr());
            bObj.pushKV("VoteCount", (int64_t)finalizedBudget->GetVoteCount());
            bObj.pushKV("Status", GetFinalizedBudgetStatus(finalizedBudget));

            bool fValid = finalizedBudget->IsValid();
            bObj.pushKV("IsValid", fValid);
            if (!fValid)
                bObj.pushKV("IsInvalidReason", finalizedBudget->IsInvalidReason());

            std::string strName = strprintf("%s (%s)", finalizedBudget->GetName(), nHash.ToString());
            resultObj.pushKV(strName, bObj);
        }

        return resultObj;
    }

    if (strCommand == "getvotes") {
        if (request.params.size() != 2)
            throw std::runtime_error("Correct usage is 'gmbudget getvotes budget-hash'");

        uint256 hash(ParseHashV(request.params[1], "budget-hash"));

        LOCK(g_budgetman.cs_budgets);
        CFinalizedBudget* pfinalBudget = g_budgetman.FindFinalizedBudget(hash);
        if (pfinalBudget == nullptr) return "Unknown budget hash";
        UniValue ret(UniValue::VOBJ);
        for (const auto& it: pfinalBudget->GetVotes()) {
            const CFinalizedBudgetVote& vote = it.second;
            ret.pushKV(vote.GetVin().prevout.ToStringShort(), vote.ToJSON());
        }
        return ret;
    }

    return NullUniValue;
}

UniValue checkbudgets(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "checkbudgets\n"
            "\nInitiates a budget check cycle manually\n"

            "\nExamples:\n" +
            HelpExampleCli("checkbudgets", "") + HelpExampleRpc("checkbudgets", ""));

    if (!g_tiertwo_sync_state.IsSynced())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Gamemaster/Budget sync not finished yet");

    g_budgetman.CheckAndRemove();
    return NullUniValue;
}

UniValue cleanbudget(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
                "cleanbudget ( try_sync )\n"
                "\nCleans the budget data manually\n"
                "\nArguments:\n"
                "1. try_sync          (boolean, optional, default=false) resets tier two sync to a pre-budget data request\n"
                "\nExamples:\n" +
                HelpExampleCli("cleanbudget", "") + HelpExampleRpc("cleanbudget", ""));

    g_budgetman.Clear();
    LogPrintf("Budget data cleaned\n");

    // reset sync if requested
    bool reset = request.params.size() > 0 ? request.params[0].get_bool() : false;
    if (reset) {
        gamemasterSync.ClearFulfilledRequest();
        gamemasterSync.Reset();
        LogPrintf("Gamemaster sync restarted\n");
    }
    return NullUniValue;
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ --------
    { "budget",             "checkbudgets",           &checkbudgets,           true,  {} },
    { "budget",             "getbudgetinfo",          &getbudgetinfo,          true,  {"name"} },
    { "budget",             "getbudgetprojection",    &getbudgetprojection,    true,  {} },
    { "budget",             "getbudgetvotes",         &getbudgetvotes,         true,  {"name"} },
    { "budget",             "getnextsuperblock",      &getnextsuperblock,      true,  {} },
    { "budget",             "gmbudgetrawvote",        &gmbudgetrawvote,        true,  {"collat_txid","collat_vout","hash","votecast","time","sig"} },
    { "budget",             "gmbudgetvote",           &gmbudgetvote,           true,  {"mode","hash","votecast","alias","legacy"} },
    { "budget",             "gmfinalbudget",          &gmfinalbudget,          true,  {"command"} },
    { "budget",             "preparebudget",          &preparebudget,          true,  {"name","url","npayments","start","address","monthly_payment"} },
    { "budget",             "submitbudget",           &submitbudget,           true,  {"name","url","npayments","start","address","monthly_payment","fee_txid"}  },

    /** Not shown in help */
    { "hidden",             "gmfinalbudgetsuggest",   &gmfinalbudgetsuggest,   true,  {} },
    { "hidden",             "createrawgmfinalbudget", &createrawgmfinalbudget, true,  {"budgetname", "blockstart", "proposals", "feetxid"} },
    { "hidden",             "cleanbudget",            &cleanbudget,            true,  {"try_sync"} },
};
// clang-format on

void RegisterBudgetRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
