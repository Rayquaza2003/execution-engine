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

int main() {
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
