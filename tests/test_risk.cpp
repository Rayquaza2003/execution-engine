#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <random>

#include "oms/oms.hpp"
#include "risk/risk.hpp"

constexpr Side B = Side::Buy;
constexpr Side Sl = Side::Sell;
constexpr OrdType L = OrdType::Limit;

static void test_each_limit_and_its_boundary() {
    RiskLimits lim;
    lim.max_order_qty = 100;
    lim.max_order_notional = 100 * 1000.0;
    lim.collar_bps = 500;  // 5%
    Risk r(2, lim);
    assert(r.check(0, B, L, 1000, 10) == Reject::NoReferencePrice);
    assert(r.check(2, B, L, 1000, 10) == Reject::UnknownSymbol);
    r.mark(0, 1000);

    assert(r.check(0, B, L, 1000, 100) == Reject::None);
    assert(r.check(0, B, L, 1000, 101) == Reject::MaxOrderQty);
    assert(r.check(0, B, L, 1050, 10) == Reject::None);  // exactly 5% away
    assert(r.check(0, B, L, 1051, 10) == Reject::FatFinger);
    assert(r.check(0, Sl, L, 949, 10) == Reject::FatFinger);
    assert(r.check(0, B, L, 1001, 100) == Reject::MaxNotional);
    assert(r.check(0, B, OrdType::Market, 0, 100) == Reject::None);  // valued at the mark, not px 0
    r.mark(0, 1001);
    assert(r.check(0, B, OrdType::Market, 0, 100) == Reject::MaxNotional);
}

static void test_position_counts_open_orders_worst_case() {
    RiskLimits lim;
    lim.max_position = 100;
    Risk r(1, lim);
    r.mark(0, 50);
    r.on_new(0, B, 60);
    assert(r.check(0, B, L, 50, 40) == Reject::None);
    assert(r.check(0, B, L, 50, 41) == Reject::MaxPosition);  // 60 open + 41
    assert(r.check(0, Sl, L, 50, 100) == Reject::None);       // sells don't add to long exposure

    r.on_fill(0, B, 50, 60);  // open 60 becomes position 60
    r.on_leaves(0, B, 60, 0, true);
    assert(r.position(0) == 60 && r.open_qty(0, B) == 0 && r.open_orders() == 0);
    assert(r.check(0, B, L, 50, 41) == Reject::MaxPosition);
    assert(r.check(0, Sl, L, 50, 160) == Reject::None);  // 60 long -> 100 short
    assert(r.check(0, Sl, L, 50, 161) == Reject::MaxPosition);

    r.on_new(0, B, 30);
    assert(r.check(0, B, L, 50, 10, 30) == Reject::None);  // replace 30 -> 10: 60 + 10
    assert(r.check(0, B, L, 50, 41, 30) == Reject::MaxPosition);
}

static void test_open_order_count() {
    RiskLimits lim;
    lim.max_open_orders = 2;
    Risk r(1, lim);
    r.mark(0, 10);
    r.on_new(0, B, 1);
    r.on_new(0, Sl, 1);
    assert(r.check(0, B, L, 10, 1) == Reject::MaxOpenOrders);
    assert(r.check(0, B, L, 10, 5, 1) == Reject::None);  // a replace adds no order
    r.on_leaves(0, Sl, 1, 0, true);
    assert(r.open_orders() == 1 && r.check(0, B, L, 10, 1) == Reject::None);
}

static void test_mark_to_market_pnl_and_loss_limit() {
    RiskLimits lim;
    lim.max_loss = 90;
    Risk r(2, lim);
    r.mark(0, 100);
    r.on_fill(0, B, 100, 10);  // long 10 @ 100
    assert(r.pnl() == 0);
    r.mark(0, 105);
    assert(r.pnl() == 50);
    r.on_fill(0, Sl, 106, 4);  // cash -1000 + 424, 6 left at 105: 54
    assert(r.pnl() == 54 && r.position(0) == 6);

    r.on_fill(1, Sl, 20, 5);  // first fill sets the mark
    r.mark(1, 50);            // short 5 loses 150
    assert(r.pnl() == 54 - 150);
    assert(r.check(0, Sl, L, 105, 6) == Reject::LossLimit);
    r.mark(1, 45);
    assert(r.pnl() == -71 && r.check(0, Sl, L, 105, 6) == Reject::None);
}

static void test_kill_switch_blocks_everything() {
    Risk r(1, {});
    r.mark(0, 10);
    assert(r.check(0, B, L, 10, 1) == Reject::None);
    r.kill();
    assert(r.killed() && r.check(0, B, L, 10, 1) == Reject::KillSwitch);
    assert(r.check(5, B, L, 10, 1) == Reject::KillSwitch);  // checked first
}

// OMS with risk attached, wired to a real venue. Counterparties use ids
// below the OMS range and account 0.
struct Rig {
    Venue v;
    SymbolId s = v.add_symbol();
    Risk risk;
    Oms oms{7, 1'000'000, &risk};
    std::vector<VenueRequest> req;
    VenueOut out;

    explicit Rig(RiskLimits lim = {}) : risk(1, lim) { risk.mark(s, 100); }
    void send() {
        for (const VenueRequest& r : req) v.on_request(r, out);
        req.clear();
        deliver();
    }
    void deliver() {
        for (const ExecReport& rep : out.reports) oms.on_report(rep);
        for (const TradePrint& p : out.prints) risk.mark(p.sym, p.price);
        out.clear();
    }
    void other(OrderId id, Side side, Price px, Qty qty) {
        v.on_new({id, 0, s, side, L, Tif::Day, px, qty}, out);
        deliver();
    }
    const OmsOrder& o(ClOrdId cl) const { return *oms.find(cl); }
    ClientOrder lim(ClOrdId cl, Side side, Price px, Qty qty) const {
        return {cl, s, side, L, Tif::Day, px, qty};
    }
};

static void test_risk_rejects_come_back_as_reports() {
    RiskLimits lim;
    lim.max_order_qty = 50;
    lim.max_position = 45;
    Rig r(lim);
    r.oms.submit(r.lim(1, B, 100, 51), r.req);
    assert(r.req.empty() && r.o(1).state == OrdState::Rejected);
    assert(r.o(1).last_reject == Reject::MaxOrderQty && r.risk.open_orders() == 0);
    assert(r.oms.submit(r.lim(1, B, 100, 5), r.req).duplicate);  // the ClOrdId is used up

    r.oms.submit(r.lim(2, B, 100, 40), r.req);
    r.send();
    assert(r.o(2).state == OrdState::Acknowledged && r.risk.open_qty(r.s, B) == 40);
    assert(!r.oms.replace(2, 100, 46, r.req) && r.req.empty());
    assert(r.o(2).state == OrdState::Acknowledged && r.o(2).last_reject == Reject::MaxPosition);
    assert(r.oms.replace(2, 100, 45, r.req));
    r.send();
    assert(r.o(2).leaves == 45 && r.risk.open_qty(r.s, B) == 45 && r.oms.invalid_transitions() == 0);
}

static void test_risk_tracks_fills_and_cancels() {
    Rig r;
    r.oms.submit(r.lim(1, B, 100, 10), r.req);
    r.oms.submit(r.lim(2, Sl, 103, 5), r.req);
    r.send();
    r.other(1, Sl, 100, 4);  // we buy 4 @ 100
    assert(r.risk.position(r.s) == 4 && r.risk.open_qty(r.s, B) == 6 && r.risk.open_orders() == 2);
    r.other(2, B, 103, 5);  // we sell 5 @ 103, mark moves to 103
    assert(r.risk.position(r.s) == -1 && r.risk.open_orders() == 1);
    assert(r.risk.pnl() == 4 * 3);  // bought 4 at 100, sold 5 at 103, short 1 marked at 103
    r.oms.cancel(1, r.req);
    r.send();
    assert(r.risk.open_qty(r.s, B) == 0 && r.risk.open_orders() == 0);
}

static void test_kill_switch_cancels_everything_working() {
    Rig r;
    r.oms.submit(r.lim(1, B, 99, 10), r.req);
    r.oms.submit(r.lim(2, Sl, 101, 10), r.req);
    r.send();
    r.oms.replace(2, 102, 8, r.req);           // replace in flight
    r.oms.submit(r.lim(3, B, 98, 5), r.req);    // new order in flight
    assert(r.oms.kill(r.req) == 3 && r.req.size() == 5);
    r.send();
    for (ClOrdId cl : {1, 2, 3}) assert(r.o(cl).state == OrdState::Cancelled);
    assert(r.risk.open_orders() == 0 && r.risk.open_qty(r.s, B) == 0 && r.risk.open_qty(r.s, Sl) == 0);
    assert(r.v.book(r.s).order_count() == 0 && r.oms.invalid_transitions() == 0);

    r.oms.submit(r.lim(4, B, 100, 1), r.req);
    assert(r.req.empty() && r.o(4).last_reject == Reject::KillSwitch);
}

// Random flow: after every step, risk's running totals must equal what the
// OMS order states say. Catches any open-qty or count update that goes missing.
static void test_risk_totals_match_oms_under_random_flow() {
    RiskLimits lim;
    lim.max_position = 300;
    lim.max_open_orders = 20;
    lim.collar_bps = 300;
    Rig r(lim);
    std::mt19937_64 rng(7);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    ClOrdId next_cl = 1;
    OrderId next_other = 1;
    size_t rejected = 0;

    for (int i = 0; i < 5000; ++i) {
        Side side = uni(0, 1) ? B : Sl;
        Price px = 100 + uni(-4, 4);
        switch (uni(0, 9)) {
            case 0: case 1: case 2: {
                const OmsOrder& o = r.oms.submit(r.lim(next_cl++, side, px, uni(1, 40)), r.req).order;
                rejected += o.state == OrdState::Rejected;
                break;
            }
            case 3: r.oms.cancel(ClOrdId(uni(1, int(next_cl))), r.req); break;
            case 4: r.oms.replace(ClOrdId(uni(1, int(next_cl))), px, uni(1, 40), r.req); break;
            default: r.other(next_other++, side, px, uni(1, 30)); break;
        }
        if (uni(0, 2) == 0) r.send();  // let requests pile up so races happen

        int64_t pos = 0;
        Qty open_b = 0, open_s = 0;
        size_t live = 0;
        for (ClOrdId cl = 1; cl < next_cl; ++cl) {
            const OmsOrder& o = r.o(cl);
            pos += o.side == B ? int64_t(o.cum) : -int64_t(o.cum);
            if (is_terminal(o.state)) continue;
            ++live;
            (o.side == B ? open_b : open_s) += o.leaves;
        }
        assert(r.risk.position(r.s) == pos && r.risk.open_orders() == live);
        assert(r.risk.open_qty(r.s, B) == open_b && r.risk.open_qty(r.s, Sl) == open_s);
        assert(open_b + std::max<int64_t>(pos, 0) <= 300 + 40);  // worst case held, give or take one replace upsize
    }
    assert(r.oms.invalid_transitions() == 0 && rejected > 0);
}

int main() {
    test_each_limit_and_its_boundary();
    test_position_counts_open_orders_worst_case();
    test_open_order_count();
    test_mark_to_market_pnl_and_loss_limit();
    test_kill_switch_blocks_everything();
    test_risk_rejects_come_back_as_reports();
    test_risk_tracks_fills_and_cancels();
    test_kill_switch_cancels_everything_working();
    test_risk_totals_match_oms_under_random_flow();
    std::puts("all tests passed");
}
