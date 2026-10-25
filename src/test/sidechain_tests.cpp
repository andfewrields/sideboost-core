// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip300/messages.h>
#include <bip300/rules.h>
#include <bip300/state.h>
#include <coins.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

using namespace bip300;

BOOST_FIXTURE_TEST_SUITE(sidechain_tests, BasicTestingSetup)

namespace {

/** Regtest's periods: activation and payout after 6 ACKs within 10 blocks.  */
constexpr Consensus::SidechainParams SHORT{
    .withdrawal_bundle_max_age = 10,
    .withdrawal_bundle_inclusion_threshold = 5,
    .used_slot_proposal_max_age = 10,
    .used_slot_activation_threshold = 5,
    .unused_slot_proposal_max_age = 10,
    .unused_slot_activation_threshold = 5,
};

const std::vector<unsigned char> DESCRIPTION{'T', 'h', 'u', 'n', 'd', 'e', 'r'};
const uint256 PREV{uint256::ONE};

CTransactionRef Coinbase(std::vector<CScript> messages)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{});
    tx.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    tx.vout.emplace_back(0, CScript() << OP_TRUE);
    for (CScript& message : messages) tx.vout.emplace_back(0, std::move(message));
    return MakeTransactionRef(std::move(tx));
}

CBlock Block(std::vector<CScript> messages, std::vector<CTransactionRef> txs = {})
{
    CBlock block;
    block.hashPrevBlock = PREV;
    block.vtx.push_back(Coinbase(std::move(messages)));
    for (auto& tx : txs) block.vtx.push_back(std::move(tx));
    return block;
}

CScript M(const CoinbaseMessage& message) { return MakeCoinbaseMessageScript(message); }

/** An ordinary transaction with `outputs`.  */
CTransactionRef Tx(std::vector<CTxOut> outputs, uint32_t n = 0)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), n});
    tx.vout = std::move(outputs);
    return MakeTransactionRef(std::move(tx));
}

/** A sidechain's blinded M6 paying `payouts`, with `fee`.  */
CTransactionRef BlindedM6(const std::vector<CAmount>& payouts, CAmount fee)
{
    CMutableTransaction tx;
    tx.version = 2;
    std::vector<unsigned char> fee_be(8);
    WriteBE64(fee_be.data(), fee);
    tx.vout.emplace_back(0, CScript() << OP_RETURN << fee_be);
    tx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(32, 7));
    for (const CAmount payout : payouts) tx.vout.emplace_back(payout, CScript() << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

/** Connect `block` at `height`; returns its reject reason, empty if valid.  */
std::string Connect(const CBlock& block, int height, State& state, const Consensus::SidechainParams& params = SHORT)
{
    BlockValidationState vstate;
    State after{state};
    // Bundles proposed before are in state.proposed, as the state isn't written
    if (!ConnectBlock(block, height, params, [](uint8_t, const uint256&) { return false; }, after, vstate)) return vstate.GetRejectReason();
    state = std::move(after);
    return "";
}

/** A state where slot 7 is active, at height 7.  */
State ActiveSlot()
{
    State state;
    BOOST_REQUIRE_EQUAL(Connect(Block({M(ProposeSidechain{7, DESCRIPTION})}), 1, state), "");
    for (int height{2}; height <= 7; ++height) {
        BOOST_REQUIRE_EQUAL(Connect(Block({M(AckSidechain{7, DescriptionHash(DESCRIPTION)})}), height, state), "");
    }
    BOOST_REQUIRE(state.FindSidechain(7));
    return state;
}

} // namespace

BOOST_AUTO_TEST_CASE(messages)
{
    const uint256 hash{m_rng.rand256()};
    for (const CoinbaseMessage& message : std::vector<CoinbaseMessage>{
             ProposeSidechain{3, DESCRIPTION},
             ProposeSidechain{3, {}},
             AckSidechain{4, hash},
             ProposeBundle{5, hash},
             AckBundles{AckBundles::Version::REPEAT_PREVIOUS, {}},
             AckBundles{AckBundles::Version::LEADING_BY_50, {}},
             MakeAckBundles({0, AckBundles::ABSTAIN, AckBundles::ALARM, 0xFD}),
             MakeAckBundles({0x100, AckBundles::ABSTAIN, AckBundles::ALARM}),
             BmmAccept{6, hash},
         }) {
        BOOST_CHECK(ParseCoinbaseMessage(MakeCoinbaseMessageScript(message)) == message);
    }
    BOOST_CHECK(MakeAckBundles({0xFD}).version == AckBundles::Version::ONE_BYTE);
    BOOST_CHECK(MakeAckBundles({0xFE}).version == AckBundles::Version::TWO_BYTES);
    // M2's tag is the enforcer's
    BOOST_CHECK_EQUAL(HexStr(M(AckSidechain{4, uint256{}})).substr(0, 14), "6a25d6e1c5df04");

    // Not messages: trailing data or ops, wrong lengths, unknown M4 versions
    CScript script{M(AckSidechain{4, hash})};
    BOOST_CHECK(!ParseCoinbaseMessage(CScript(script) << OP_TRUE));
    std::vector<unsigned char> data(M2_TAG.begin(), M2_TAG.end());
    data.resize(4 + 1 + 31);
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << data));
    data.resize(4 + 1 + 33);
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << data));
    std::vector<unsigned char> m4(M4_TAG.begin(), M4_TAG.end());
    m4.push_back(4);
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << m4));
    m4.back() = 0;
    m4.push_back(0);
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << m4)); // REPEAT_PREVIOUS takes no votes
    m4[4] = 2;
    BOOST_CHECK(!ParseCoinbaseMessage(CScript() << OP_RETURN << m4)); // odd TWO_BYTES

    // Deposits
    const CScript deposit{MakeDepositScript(9, DESCRIPTION)};
    BOOST_CHECK(IsDepositScript(deposit));
    BOOST_CHECK(ParseDeposit(CTxOut{1, deposit}) == (Deposit{9, DESCRIPTION}));
    BOOST_CHECK(!ParseDeposit(CTxOut{0, deposit}));
    BOOST_CHECK(!ParseDeposit(CTxOut{1, CScript(deposit) << OP_TRUE}));
    BOOST_CHECK(IsDepositScript(CScript(deposit) << OP_TRUE));
    BOOST_CHECK(!ParseDeposit(CTxOut{1, CScript() << OP_RETURN << std::vector<unsigned char>(M5_TAG.begin(), M5_TAG.end())}));
    BOOST_CHECK(!IsDepositScript(M(ProposeSidechain{3, DESCRIPTION})));

    // BMM requests: the first output only
    const BmmRequest request{2, hash, m_rng.rand256()};
    BOOST_CHECK(ParseBmmRequest(*Tx({CTxOut{0, MakeBmmRequestScript(request)}})) == request);
    BOOST_CHECK(!ParseBmmRequest(*Tx({CTxOut{0, CScript() << OP_TRUE}, CTxOut{0, MakeBmmRequestScript(request)}})));
    BOOST_CHECK_EQUAL(MakeBmmRequestScript(request).size(), 70U);

    // Payouts: the blinded M6 plus the treasury input
    const CTransactionRef blinded{BlindedM6({1000, 2000}, 300)};
    BOOST_CHECK_EQUAL(ParseM6Fee(*blinded).value(), 300);
    BOOST_CHECK_EQUAL(M6Value(*blinded).value(), 3300);
    const CTransactionRef payout{MakePayout(*blinded, 5)};
    BOOST_CHECK(payout->IsSidechainPayout());
    BOOST_CHECK(!blinded->IsSidechainPayout());
    BOOST_CHECK(ComputeM6Id(*payout) == blinded->GetHash().ToUint256());
    BOOST_CHECK(!M6Value(*payout));
    BOOST_CHECK(!ParseM6Fee(*Tx({CTxOut{1, CScript() << OP_TRUE}})));
}

BOOST_AUTO_TEST_CASE(activation)
{
    const uint256 desc_hash{DescriptionHash(DESCRIPTION)};
    State state;
    // Proposed and ACKed in the same block: the ACK doesn't count
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeSidechain{7, DESCRIPTION}), M(AckSidechain{7, desc_hash})}), 1, state), "");
    BOOST_REQUIRE_EQUAL(state.proposals.size(), 1U);
    BOOST_CHECK_EQUAL(state.proposals[0].vote_count, 0U);
    // Proposing it again doesn't reset it; twice in a block is invalid
    BOOST_CHECK_EQUAL(Connect(Block({M(AckSidechain{7, desc_hash})}), 2, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeSidechain{7, DESCRIPTION})}), 3, state), "");
    BOOST_CHECK_EQUAL(state.proposals.size(), 1U);
    BOOST_CHECK_EQUAL(state.proposals[0].vote_count, 1U);
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeSidechain{7, DESCRIPTION}), M(ProposeSidechain{7, DESCRIPTION})}), 3, state), "bad-cb-sidechain-proposal-duplicate");
    BOOST_CHECK_EQUAL(Connect(Block({M(AckSidechain{7, desc_hash}), M(AckSidechain{7, uint256::ONE})}), 3, state), "bad-cb-sidechain-ack-duplicate");
    // ACKs for another slot, or no proposal, are ignored
    BOOST_CHECK_EQUAL(Connect(Block({M(AckSidechain{8, desc_hash})}), 3, state), "");
    BOOST_CHECK_EQUAL(state.proposals[0].vote_count, 1U);
    // More than 5 ACKs activate it
    for (int height{4}; height <= 8; ++height) {
        BOOST_CHECK(!state.FindSidechain(7));
        BOOST_CHECK_EQUAL(Connect(Block({M(AckSidechain{7, desc_hash})}), height, state), "");
    }
    const Sidechain* sidechain{state.FindSidechain(7)};
    BOOST_REQUIRE(sidechain);
    BOOST_CHECK_EQUAL(sidechain->vote_count, 6U);
    BOOST_CHECK_EQUAL(sidechain->proposal_height, 1);
    BOOST_CHECK_EQUAL(sidechain->activation_height, 8);
    BOOST_CHECK(state.proposals.empty());
    BOOST_CHECK(state.bundles.at(7).empty());

    // A proposal fails once it can't make it: more than 10 blocks old, or
    // more than 5 (max age - threshold) with as many blocks not ACKing it
    State failing;
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeSidechain{1, DESCRIPTION})}), 1, failing), "");
    for (int height{2}; height <= 6; ++height) BOOST_CHECK_EQUAL(Connect(Block({}), height, failing), "");
    BOOST_CHECK_EQUAL(failing.proposals.size(), 1U);
    BOOST_CHECK_EQUAL(Connect(Block({}), 7, failing), "");
    BOOST_CHECK(failing.proposals.empty());
}

BOOST_AUTO_TEST_CASE(replacement)
{
    State state{ActiveSlot()};
    const std::vector<unsigned char> other{'Z', 'e', 'r', 'o'};
    state.treasuries[7] = {.value = 500, .next_sequence = 3};
    state.bundles[7].push_back({.m6id = uint256::ONE, .vote_count = 1, .proposal_height = 7});
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeSidechain{7, other})}), 8, state), "");
    for (int height{9}; height <= 14; ++height) {
        BOOST_CHECK_EQUAL(Connect(Block({M(AckSidechain{7, DescriptionHash(other)})}), height, state), "");
    }
    // The used slot's thresholds are the same on regtest: replaced
    BOOST_CHECK(state.FindSidechain(7)->description_hash == DescriptionHash(other));
    BOOST_CHECK((state.treasuries.at(7) == Treasury{.value = 500, .next_sequence = 3}));
}

BOOST_AUTO_TEST_CASE(deposits)
{
    State state{ActiveSlot()};
    BOOST_CHECK_EQUAL(Connect(Block({}, {Tx({CTxOut{1000, MakeDepositScript(7, DESCRIPTION)}, CTxOut{1, MakeDepositScript(7, {})}})}), 8, state), "");
    BOOST_CHECK((state.treasuries.at(7) == Treasury{.value = 1001, .next_sequence = 2}));
    BOOST_CHECK_EQUAL(Connect(Block({}, {Tx({CTxOut{1000, MakeDepositScript(8, DESCRIPTION)}})}), 9, state), "bad-txns-sidechain-deposit-inactive");
    BOOST_CHECK_EQUAL(Connect(Block({}, {Tx({CTxOut{0, MakeDepositScript(7, DESCRIPTION)}})}), 9, state), "bad-txns-sidechain-deposit");
    BOOST_CHECK_EQUAL(Connect(Block({MakeDepositScript(7, DESCRIPTION)}), 9, state), "bad-cb-sidechain-deposit");
}

BOOST_AUTO_TEST_CASE(bundles)
{
    State state{ActiveSlot()};
    state.treasuries[7] = {.value = 10'000, .next_sequence = 1};
    const CTransactionRef blinded{BlindedM6({1000, 2000}, 300)};
    const uint256 m6id{blinded->GetHash().ToUint256()};

    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{8, m6id})}), 8, state), "bad-cb-bundle-inactive-slot");
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{7, m6id})}), 8, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{7, m6id})}), 9, state), "bad-cb-bundle-duplicate");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 1U);

    // Votes must match the active slots and their bundles
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0, 0}))}), 9, state), "bad-cb-bundle-votes");
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({1}))}), 9, state), "bad-cb-bundle-votes");
    BOOST_CHECK_EQUAL(Connect(Block({M(AckBundles{AckBundles::Version::TWO_BYTES, {0}})}), 9, state), "bad-cb-bundle-votes");
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0})), M(MakeAckBundles({0}))}), 9, state), "bad-cb-bundle-votes-duplicate");

    // Upvotes, repeated
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}), 9, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(AckBundles{AckBundles::Version::REPEAT_PREVIOUS, {}})}), 10, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 3U);
    // An alarm takes one off
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({AckBundles::ALARM}))}), 11, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 2U);
    // No M4: nothing to repeat
    BOOST_CHECK_EQUAL(Connect(Block({}), 12, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(AckBundles{AckBundles::Version::REPEAT_PREVIOUS, {}})}), 13, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 2U);
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}), 13, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}), 14, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}), 15, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 5U);

    // The sixth vote takes it over the threshold: the block must pay it out,
    // and nothing else
    const CTransactionRef payout{MakePayout(*blinded, 7)};
    const CBlock voting{Block({M(MakeAckBundles({0}))})};
    BOOST_CHECK_EQUAL(Connect(voting, 16, state), "bad-blk-sidechain-payout-missing");
    BOOST_CHECK_EQUAL(Connect(Block({}, {payout}), 16, state), "bad-txns-sidechain-payout-unapproved");
    const CTransactionRef other{MakePayout(*BlindedM6({1000}, 300), 7)};
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}, {other}), 16, state), "bad-txns-sidechain-payout-unapproved");
    CMutableTransaction signed_payout{*payout};
    signed_payout.vin[0].scriptSig = CScript() << OP_TRUE;
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}, {MakeTransactionRef(signed_payout)}), 16, state), "bad-txns-sidechain-payout");
    State poor{state};
    poor.treasuries[7].value = 3299;
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}, {payout}), 16, poor), "bad-txns-sidechain-payout-treasury");
    // A deposit earlier in the block makes up for it
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}, {Tx({CTxOut{1, MakeDepositScript(7, {})}}), payout}), 16, poor), "");
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({0}))}, {payout}), 16, state), "");
    BOOST_CHECK((state.treasuries.at(7) == Treasury{.value = 10'000 - 3300, .next_sequence = 2}));
    BOOST_CHECK(state.bundles.at(7).empty());
    // Paid out: no longer pending, so not payable again
    BOOST_CHECK_EQUAL(Connect(Block({}, {payout}), 17, state), "bad-txns-sidechain-payout-unapproved");
}

BOOST_AUTO_TEST_CASE(competing_bundles)
{
    State state{ActiveSlot()};
    const uint256 a{uint256::ONE}, b{m_rng.rand256()};
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{7, a}), M(ProposeBundle{7, b})}), 8, state), "");
    // Upvoting one downvotes the other
    BOOST_CHECK_EQUAL(Connect(Block({M(MakeAckBundles({1}))}), 9, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(0).vote_count, 0U);
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(1).vote_count, 2U);
    // Votes in the same coinbase after an M3 see it
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{7, uint256{2}}), M(MakeAckBundles({2}))}), 10, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(2).vote_count, 2U);
    BOOST_CHECK_EQUAL(state.bundles.at(7).at(1).vote_count, 1U);
    // Bundles expire after 10 blocks
    BOOST_CHECK_EQUAL(Connect(Block({}), 18, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).size(), 3U);
    BOOST_CHECK_EQUAL(Connect(Block({}), 19, state), "");
    BOOST_CHECK_EQUAL(state.bundles.at(7).size(), 1U);
    // A bundle is proposed only once
    BOOST_CHECK_EQUAL(Connect(Block({M(ProposeBundle{7, a})}), 20, state), "bad-cb-bundle-reproposed");
    BlockValidationState vstate;
    State fresh{ActiveSlot()};
    BOOST_CHECK(!ConnectBlock(Block({M(ProposeBundle{7, a})}), 8, SHORT, [&](uint8_t slot, const uint256& m6id) { return slot == 7 && m6id == a; }, fresh, vstate));
    BOOST_CHECK_EQUAL(vstate.GetRejectReason(), "bad-cb-bundle-reproposed");
}

BOOST_AUTO_TEST_CASE(leading_by_50)
{
    State state{ActiveSlot()};
    state.bundles[7] = {{.m6id = uint256::ONE, .vote_count = 60, .proposal_height = 7}, {.m6id = uint256{2}, .vote_count = 10, .proposal_height = 7}};
    const AckBundles m4{AckBundles::Version::LEADING_BY_50, {}};
    BOOST_CHECK((ResolveVotes(m4, state) == std::vector<Vote>{{7, Vote::Kind::UPVOTE, uint256::ONE}}));
    state.bundles[7][1].vote_count = 11;
    BOOST_CHECK(ResolveVotes(m4, state)->empty());
    state.bundles[7].pop_back();
    state.bundles[7][0].vote_count = 50;
    BOOST_CHECK_EQUAL(ResolveVotes(m4, state)->size(), 1U);
    // Repeated, it isn't evaluated again: the upvote is cast as it was
    state.last_votes = *ResolveVotes(m4, state);
    state.bundles[7][0].vote_count = 1;
    BOOST_CHECK((ResolveVotes(AckBundles{AckBundles::Version::REPEAT_PREVIOUS, {}}, state) == state.last_votes));
}

BOOST_AUTO_TEST_CASE(bmm)
{
    State state;
    const uint256 h{m_rng.rand256()};
    const CTransactionRef request{Tx({CTxOut{0, MakeBmmRequestScript({3, h, PREV})}})};
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, h})}, {request}), 1, state), "");
    BOOST_CHECK_EQUAL(Connect(Block({}, {request}), 1, state), "bad-txns-bmm-request-unaccepted");
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, uint256::ONE})}, {request}), 1, state), "bad-txns-bmm-request-unaccepted");
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, h}), M(BmmAccept{3, h})}), 1, state), "bad-cb-bmm-accept-duplicate");
    const CTransactionRef stale{Tx({CTxOut{0, MakeBmmRequestScript({3, h, uint256::ZERO})}}, 1)};
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, h})}, {stale}), 1, state), "bad-txns-bmm-request-prev");
    const CTransactionRef second{Tx({CTxOut{0, MakeBmmRequestScript({3, h, PREV})}}, 2)};
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, h})}, {request, second}), 1, state), "bad-txns-bmm-request-duplicate");
    // An M7 needs no request
    BOOST_CHECK_EQUAL(Connect(Block({M(BmmAccept{3, h})}), 1, state), "");
}

BOOST_AUTO_TEST_CASE(state_coins)
{
    CCoinsViewCache view{&CoinsViewEmpty::Get()};
    const State empty;
    BOOST_CHECK(*ReadState(view) == empty);
    // Nothing happened: no coins
    BOOST_CHECK(WriteState(view, empty, empty, 1).empty());

    State state{ActiveSlot()};
    state.treasuries[7] = {.value = 1234, .next_sequence = 5};
    state.bundles[7].push_back({.m6id = uint256::ONE, .vote_count = 3, .proposal_height = 4});
    state.last_votes.push_back({7, Vote::Kind::UPVOTE, uint256::ONE});
    // Enough proposals to take several coins
    for (int i{0}; i < 300; ++i) state.proposals.push_back({.slot = uint8_t(i), .description_hash = m_rng.rand256(), .vote_count = uint32_t(i), .proposal_height = i});
    const auto changes{WriteState(view, empty, state, 8)};
    BOOST_CHECK(*ReadState(view) == state);
    BOOST_CHECK(view.AccessCoin(COutPoint{StateTxid(Part::PROPOSALS), 1}).out.nValue == 0);
    BOOST_CHECK(view.AccessCoin(COutPoint{SidechainTreasuryTxid(), 7}).out.nValue == 1234);

    // Changes, recorded in undo data, undo
    State next{state};
    next.proposals.resize(10);
    next.treasuries[7].value = 0;
    std::vector<StateCoinChange> later{WriteState(view, state, next, 9)};
    BOOST_CHECK(*ReadState(view) == next);
    BOOST_CHECK(view.AccessCoin(COutPoint{StateTxid(Part::PROPOSALS), 1}).IsSpent());
    CBlockUndo undo;
    undo.vstate = later;
    DataStream stream;
    stream << undo;
    CBlockUndo read;
    stream >> read;
    BOOST_REQUIRE(UndoStateChanges(view, read.vstate));
    BOOST_CHECK(*ReadState(view) == state);
    BOOST_CHECK(!UndoStateChanges(view, read.vstate)); // not applied any more
    BOOST_REQUIRE(UndoStateChanges(view, changes));
    BOOST_CHECK(*ReadState(view) == empty);
    BOOST_CHECK(view.AccessCoin(COutPoint{SidechainTreasuryTxid(), 7}).IsSpent());

    // Proposed bundles get a coin each
    State proposing{state};
    proposing.proposed.emplace_back(7, uint256::ONE);
    const auto proposal_changes{WriteState(view, state, proposing, 9)};
    BOOST_CHECK_EQUAL(proposal_changes.size(), 1U);
    BOOST_CHECK(view.HaveCoin(ProposedBundleOutpoint(7, uint256::ONE)));
    BOOST_CHECK(!view.HaveCoin(ProposedBundleOutpoint(8, uint256::ONE)));
    BOOST_REQUIRE(UndoStateChanges(view, proposal_changes));
    BOOST_CHECK(!view.HaveCoin(ProposedBundleOutpoint(7, uint256::ONE)));

    // Corrupt state is reported
    view.AddCoin(COutPoint{StateTxid(Part::SIDECHAINS), 0}, Coin{CTxOut{0, CScript() << std::vector<unsigned char>{1} << OP_DROP << OP_FALSE}, 1, false}, false);
    BOOST_CHECK(!ReadState(view));
}

BOOST_AUTO_TEST_SUITE_END()
