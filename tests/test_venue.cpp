#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <vector>

#include "exchange/venue.hpp"

static NewOrder lim(OrderId id, SymbolId sym, Side side, Price px, Qty qty, Tif tif = Tif::Day,
                    AccountId acct = 0) {
    return {id, acct, sym, side, OrdType::Limit, tif, px, qty};
}

static NewOrder mkt(OrderId id, SymbolId sym, Side side, Qty qty) {
    return {id, 0, sym, side, OrdType::Market, Tif::Ioc, 0, qty};
}

static std::vector<ExecType> types(const VenueOut& o) {
    std::vector<ExecType> v;
    for (const ExecReport& r : o.reports) v.push_back(r.type);
    return v;
}

using E = ExecType;

static void test_unknown_symbol() {
    Venue v;
    VenueOut o;
    v.on_new(lim(1, 0, Side::Buy, 100, 1), o);
    assert(o.reports.size() == 1 && o.reports[0].type == E::Rejected &&
           o.reports[0].reason == Reject::UnknownSymbol);
    v.on_cancel({1, 3}, o);
    assert(o.reports[1].type == E::CancelRejected);
}

static void test_symbols_are_independent() {
    Venue v;
    VenueOut o;
    SymbolId a = v.add_symbol(), m = v.add_symbol();
    v.on_new(lim(1, a, Side::Sell, 100, 5), o);
    v.on_new(lim(2, m, Side::Buy, 100, 5), o);  // same price, different symbol: no trade
    assert(o.prints.empty());
    assert(*v.book(a).best_ask() == 100 && !v.book(a).best_bid());
    assert(*v.book(m).best_bid() == 100 && !v.book(m).best_ask());
    assert(o.levels.size() == 2 && o.levels[0].sym == a && o.levels[1].sym == m);
}

static void test_resting_ack() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Buy, 100, 7), o);
    assert((types(o) == std::vector<E>{E::New}));
    assert(o.reports[0].leaves == 7);
    assert(o.levels.size() == 1 && o.levels[0].level.price == 100 && o.levels[0].level.qty == 7);
}

static void test_cross_reports_both_sides_and_prints() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Sell, 100, 3), o);
    v.on_new(lim(2, s, Side::Sell, 101, 3), o);
    o.clear();

    v.on_new(lim(3, s, Side::Buy, 101, 5), o);
    assert((types(o) == std::vector<E>{E::New, E::Trade, E::Trade, E::Trade, E::Trade}));
    const auto& r = o.reports;
    assert(r[1].id == 3 && r[1].last_px == 100 && r[1].last_qty == 3 && r[1].leaves == 2);
    assert(r[2].id == 1 && r[2].leaves == 0);
    assert(r[3].id == 3 && r[3].last_px == 101 && r[3].last_qty == 2 && r[3].leaves == 0);
    assert(r[4].id == 2 && r[4].leaves == 1);
    assert(o.prints.size() == 2 && o.prints[0].aggressor == Side::Buy && o.prints[1].qty == 2);
    assert(o.levels.size() == 2 && o.levels[0].level.qty == 0 && o.levels[1].level.qty == 1);
}

static void test_ioc_and_market_remainders_cancel() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Sell, 100, 2), o);
    o.clear();
    v.on_new(lim(2, s, Side::Buy, 100, 5, Tif::Ioc), o);
    assert((types(o) == std::vector<E>{E::New, E::Trade, E::Trade, E::Cancelled}));
    assert(o.reports.back().id == 2 && o.reports.back().leaves == 0);

    o.clear();
    v.on_new(mkt(3, s, Side::Sell, 4), o);  // empty book
    assert((types(o) == std::vector<E>{E::New, E::Cancelled}));
}

static void test_cancel_and_replace() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Buy, 99, 5), o);
    v.on_new(lim(2, s, Side::Sell, 101, 3), o);
    o.clear();

    v.on_replace({1, s, 101, 5}, o);  // reprice through the ask: trades 3, 2 rest
    assert((types(o) == std::vector<E>{E::Replaced, E::Trade, E::Trade}));
    assert(o.reports[1].id == 1 && o.reports[1].leaves == 2);
    assert(v.book(s).depth(Side::Buy, 101) == 2);

    o.clear();
    v.on_replace({1, s, 101 + OrderBook::kMaxSpan, 5}, o);
    assert(o.reports[0].type == E::ReplaceRejected && o.reports[0].reason == Reject::PriceBand);
    assert(v.book(s).leaves(1) == 2);

    o.clear();
    v.on_cancel({1, s}, o);
    v.on_cancel({1, s}, o);
    assert((types(o) == std::vector<E>{E::Cancelled, E::CancelRejected}));
    assert(o.reports[1].reason == Reject::UnknownOrder);
}

static void test_rejects_and_stp_reach_the_client() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Buy, 100, 0), o);
    assert(o.reports[0].type == E::Rejected && o.reports[0].reason == Reject::ZeroQty);

    o.clear();
    v.on_new(lim(2, s, Side::Sell, 100, 5, Tif::Day, 7), o);
    v.on_new(lim(3, s, Side::Buy, 100, 5, Tif::Day, 7), o);
    assert((types(o) == std::vector<E>{E::New, E::New, E::Cancelled}));
    assert(o.reports[2].reason == Reject::SelfTrade && o.prints.empty());
}

static void test_end_of_day() {
    Venue v;
    VenueOut o;
    SymbolId a = v.add_symbol(), b = v.add_symbol();
    v.on_new(lim(1, a, Side::Buy, 100, 5), o);
    v.on_new(lim(2, b, Side::Sell, 100, 5, Tif::Gtc), o);
    v.on_new(lim(3, b, Side::Sell, 101, 5), o);
    o.clear();
    v.end_of_day(o);
    assert((types(o) == std::vector<E>{E::Expired, E::Expired}));
    assert(o.reports[0].id == 1 && o.reports[0].sym == a && o.reports[1].id == 3 && o.reports[1].sym == b);
    assert(v.book(b).leaves(2) == 5 && o.levels.size() == 2);
}

static NewOrder stop(OrderId id, SymbolId sym, Side side, Price stop_px, Price px, Qty qty,
                     Tif tif = Tif::Day) {
    NewOrder o{id, 0, sym, side, OrdType::StopLimit, tif, px, qty};
    o.stop_px = stop_px;
    return o;
}

static void test_buy_stop_triggers_at_or_above() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(stop(1, s, Side::Buy, 102, 105, 4), o);
    assert((types(o) == std::vector<E>{E::New}) && v.book(s).order_count() == 0);

    v.on_new(lim(2, s, Side::Sell, 101, 1), o);
    v.on_new(lim(3, s, Side::Buy, 101, 1), o);  // prints 101: below trigger
    assert(o.reports.back().type == E::Trade);

    v.on_new(lim(4, s, Side::Sell, 102, 1), o);
    v.on_new(lim(5, s, Side::Sell, 104, 10), o);
    o.clear();
    v.on_new(lim(6, s, Side::Buy, 102, 1), o);  // prints 102: fires the stop
    assert((types(o) == std::vector<E>{E::New, E::Trade, E::Trade, E::Triggered, E::Trade, E::Trade}));
    assert(o.reports[3].id == 1 && o.reports[4].id == 1 && o.reports[4].last_px == 104 &&
           o.reports[4].last_qty == 4 && o.reports[4].leaves == 0);
}

static void test_sell_stop_and_cascade() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Buy, 99, 2), o);
    v.on_new(lim(2, s, Side::Buy, 97, 5), o);
    v.on_new(stop(10, s, Side::Sell, 99, 90, 2), o);  // fires at <= 99, sells into 97
    v.on_new(stop(11, s, Side::Sell, 97, 90, 1), o);  // fires once 97 prints
    o.clear();
    v.on_new(mkt(3, s, Side::Sell, 1), o);  // prints 99
    std::vector<E> want{E::New, E::Trade, E::Trade,
                        E::Triggered, E::Trade, E::Trade, E::Trade, E::Trade,  // 10: 1@99, 1@97
                        E::Triggered, E::Trade, E::Trade};                     // 11: 1@97
    assert(types(o) == want);
    assert(o.reports[3].id == 10 && o.reports[8].id == 11);
    assert(v.book(s).depth(Side::Buy, 97) == 3);
}

static void test_stop_already_through_fires_on_arrival() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Sell, 100, 5), o);
    v.on_new(lim(2, s, Side::Buy, 100, 1), o);  // last = 100
    o.clear();
    v.on_new(stop(3, s, Side::Buy, 99, 100, 2), o);
    assert((types(o) == std::vector<E>{E::New, E::Triggered, E::Trade, E::Trade}));
}

static void test_stop_cancel_replace_dup_and_expiry() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(stop(1, s, Side::Buy, 110, 111, 3), o);
    v.on_new(stop(2, s, Side::Sell, 90, 89, 3, Tif::Gtc), o);
    v.on_new(stop(3, s, Side::Buy, 110, 111, 3), o);
    v.on_new(lim(1, s, Side::Buy, 100, 1), o);  // id taken by a waiting stop
    assert(o.reports.back().type == E::Rejected && o.reports.back().reason == Reject::DuplicateId);

    o.clear();
    v.on_replace({3, s, 112, 7}, o);
    v.on_cancel({1, s}, o);
    v.on_cancel({1, s}, o);
    assert((types(o) == std::vector<E>{E::Replaced, E::Cancelled, E::CancelRejected}));

    o.clear();
    v.end_of_day(o);  // Day stop 3 expires, Gtc stop 2 stays
    assert((types(o) == std::vector<E>{E::Expired}) && o.reports[0].id == 3);
    v.on_cancel({2, s}, o);
    assert(o.reports.back().type == E::Cancelled);
}

static void test_breaker_off_by_default() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Sell, 100, 1), o);
    v.on_new(lim(2, s, Side::Buy, 100, 1), o);
    v.on_new(lim(3, s, Side::Sell, 200, 1), o);
    v.on_new(lim(4, s, Side::Buy, 200, 1), o);  // +100%
    assert(!v.halted(s));
}

static void test_breaker_halts_and_resumes() {
    constexpr Nanos kSec = 1'000'000'000;
    Venue v({500, 60 * kSec, 300 * kSec});  // 5% band, 1 min window, 5 min halt
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_new(lim(1, s, Side::Sell, 100, 1), o);
    v.on_new(lim(2, s, Side::Buy, 100, 1), o);  // reference = 100
    v.on_new(lim(3, s, Side::Sell, 105, 1), o);
    v.on_new(lim(4, s, Side::Buy, 105, 1), o);  // exactly 5%: still inside the band
    assert(!v.halted(s));

    v.on_new(lim(5, s, Side::Buy, 90, 1), o);
    v.on_new(stop(6, s, Side::Buy, 106, 120, 1), o);  // would fire on the next print
    v.on_new(lim(7, s, Side::Sell, 106, 2), o);
    v.set_time(10 * kSec);
    v.on_new(lim(8, s, Side::Buy, 106, 1), o);  // 6% above 100: halt
    assert(v.halted(s));
    assert(o.reports.back().type != E::Triggered);  // stops do not fire into a halt

    o.clear();
    v.on_new(lim(9, s, Side::Buy, 106, 1), o);
    v.on_replace({5, s, 91, 1}, o);
    v.on_cancel({5, s}, o);  // cancels still work while halted
    assert((types(o) == std::vector<E>{E::Rejected, E::ReplaceRejected, E::Cancelled}));
    assert(o.reports[0].reason == Reject::Halted && o.reports[1].reason == Reject::Halted);

    v.set_time(310 * kSec);
    assert(!v.halted(s));
    o.clear();
    v.on_new(lim(10, s, Side::Buy, 106, 1), o);  // resumes; the waiting stop fires off this print
    assert(o.reports[0].type == E::New && !v.halted(s));
    bool triggered = false;
    for (auto& r : o.reports) triggered |= r.type == E::Triggered && r.id == 6;
    assert(triggered);
}

static void test_breaker_reference_rolls_with_window() {
    constexpr Nanos kSec = 1'000'000'000;
    Venue v({500, 60 * kSec, 300 * kSec});
    VenueOut o;
    SymbolId s = v.add_symbol();
    OrderId id = 1;
    Price px = 100;
    for (int minute = 0; minute < 5; ++minute) {  // +4% per minute: never 5% from the reference
        v.set_time(Nanos(minute) * 61 * kSec);
        v.on_new(lim(id++, s, Side::Sell, px, 1), o);
        v.on_new(lim(id++, s, Side::Buy, px, 1), o);
        assert(!v.halted(s));
        px = px * 104 / 100;
    }
}

static void test_trade_history() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    for (OrderId i = 0; i < 1500; ++i) {
        v.on_new(lim(2 * i + 1, s, Side::Sell, 100 + Price(i % 7), 1), o);
        v.on_new(lim(2 * i + 2, s, Side::Buy, 100 + Price(i % 7), 1), o);
    }
    const auto& h = v.trades(s);
    assert(h.total_pushed() == 1500 && h.size() == Venue::kTradeHistory);
    assert(h.newest().price == 100 + 1499 % 7 && h[0].price == 100 + (1500 - 1024) % 7);
}

static void test_on_request_dispatch() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.on_request(lim(1, s, Side::Buy, 100, 5), o);
    v.on_request(ReplaceRequest{1, s, 101, 4}, o);
    v.on_request(CancelRequest{1, s}, o);
    assert((types(o) == std::vector<E>{E::New, E::Replaced, E::Cancelled}));
}

// Replay with our orders mixed in: a feed execution fills ours first when
// we rank ahead of the order it names, and never when we are behind it.
static void test_replay_fills_our_orders_ahead() {
    Venue v;
    VenueOut o;
    SymbolId s = v.add_symbol();
    v.replay_add(s, 100, Side::Buy, 50, 10, o);                    // feed A
    v.on_new(lim(1, s, Side::Buy, 50, 4, Tif::Day, 7), o);        // ours, behind A
    v.replay_add(s, 101, Side::Buy, 50, 10, o);                   // feed B, behind us
    o.clear();

    v.replay_execute(s, 100, 6, std::nullopt, true, o);  // A is ahead of us: nothing for us
    assert(o.reports.empty() && o.prints.size() == 1 && v.book(s).leaves(1) == 4);
    o.clear();
    v.replay_execute(s, 101, 3, std::nullopt, true, o);  // B is behind us: we fill 3 first
    assert(o.reports.size() == 1 && o.reports[0].id == 1 && o.reports[0].type == E::Trade);
    assert(o.reports[0].last_px == 50 && o.reports[0].last_qty == 3 && o.reports[0].leaves == 1);
    assert(o.prints.size() == 2 && o.prints[0].aggressor == Side::Sell);
    assert(v.book(s).leaves(101) == 7 && v.book(s).depth(Side::Buy, 50) == 4 + 1 + 7);

    // Trade-through: our better-priced offer fills before a feed offer at a worse price.
    v.replay_add(s, 102, Side::Sell, 53, 10, o);
    v.on_new(lim(2, s, Side::Sell, 52, 5, Tif::Day, 7), o);
    o.clear();
    v.replay_execute(s, 102, 8, std::nullopt, true, o);
    assert(o.reports.size() == 1 && o.reports[0].id == 2 && o.reports[0].last_px == 52 &&
           o.reports[0].last_qty == 5 && o.reports[0].leaves == 0);
    assert(!v.book(s).leaves(2) && v.book(s).leaves(102) == 2 && *v.book(s).best_ask() == 53);
}

int main() {
    test_replay_fills_our_orders_ahead();
    test_on_request_dispatch();
    test_trade_history();
    test_breaker_off_by_default();
    test_breaker_halts_and_resumes();
    test_breaker_reference_rolls_with_window();
    test_buy_stop_triggers_at_or_above();
    test_sell_stop_and_cascade();
    test_stop_already_through_fires_on_arrival();
    test_stop_cancel_replace_dup_and_expiry();
    test_unknown_symbol();
    test_symbols_are_independent();
    test_resting_ack();
    test_cross_reports_both_sides_and_prints();
    test_ioc_and_market_remainders_cancel();
    test_cancel_and_replace();
    test_rejects_and_stp_reach_the_client();
    test_end_of_day();
    std::puts("all tests passed");
}
