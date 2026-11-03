#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

#include "oms/oms.hpp"

using S = OrdState;
using E = OrdEvent;

struct Rule {
    S from;
    E event;
    S to_unfilled;  // next state when nothing has filled yet
    S to_filled;    // next state when something has
};

// Independent spec of every valid transition. Anything not listed must be invalid.
static const std::vector<Rule> kSpec = {
    {S::PendingNew, E::Ack, S::Acknowledged, S::Acknowledged},
    {S::PendingNew, E::Rejected, S::Rejected, S::Rejected},
    {S::PendingNew, E::CancelReq, S::CancelPending, S::CancelPending},

    {S::Acknowledged, E::PartialFill, S::PartiallyFilled, S::PartiallyFilled},
    {S::Acknowledged, E::Fill, S::Filled, S::Filled},
    {S::Acknowledged, E::Cancelled, S::Cancelled, S::Cancelled},
    {S::Acknowledged, E::Expired, S::Expired, S::Expired},
    {S::Acknowledged, E::Rejected, S::Rejected, S::Rejected},
    {S::Acknowledged, E::Triggered, S::Acknowledged, S::Acknowledged},
    {S::Acknowledged, E::CancelReq, S::CancelPending, S::CancelPending},
    {S::Acknowledged, E::ReplaceReq, S::ReplacePending, S::ReplacePending},

    {S::PartiallyFilled, E::PartialFill, S::PartiallyFilled, S::PartiallyFilled},
    {S::PartiallyFilled, E::Fill, S::Filled, S::Filled},
    {S::PartiallyFilled, E::Cancelled, S::Cancelled, S::Cancelled},
    {S::PartiallyFilled, E::Expired, S::Expired, S::Expired},
    {S::PartiallyFilled, E::Rejected, S::Rejected, S::Rejected},
    {S::PartiallyFilled, E::Triggered, S::PartiallyFilled, S::PartiallyFilled},
    {S::PartiallyFilled, E::CancelReq, S::CancelPending, S::CancelPending},
    {S::PartiallyFilled, E::ReplaceReq, S::ReplacePending, S::ReplacePending},

    {S::CancelPending, E::Ack, S::CancelPending, S::CancelPending},
    {S::CancelPending, E::Triggered, S::CancelPending, S::CancelPending},
    {S::CancelPending, E::PartialFill, S::CancelPending, S::CancelPending},
    {S::CancelPending, E::Fill, S::Filled, S::Filled},
    {S::CancelPending, E::Cancelled, S::Cancelled, S::Cancelled},
    {S::CancelPending, E::Expired, S::Expired, S::Expired},
    {S::CancelPending, E::Rejected, S::Rejected, S::Rejected},
    {S::CancelPending, E::CancelRejected, S::Acknowledged, S::PartiallyFilled},

    {S::ReplacePending, E::Replaced, S::Acknowledged, S::PartiallyFilled},
    {S::ReplacePending, E::ReplaceRejected, S::Acknowledged, S::PartiallyFilled},
    {S::ReplacePending, E::Triggered, S::ReplacePending, S::ReplacePending},
    {S::ReplacePending, E::PartialFill, S::ReplacePending, S::ReplacePending},
    {S::ReplacePending, E::Fill, S::Filled, S::Filled},
    {S::ReplacePending, E::Cancelled, S::Cancelled, S::Cancelled},
    {S::ReplacePending, E::Expired, S::Expired, S::Expired},
};

static void test_every_state_event_pair_matches_spec() {
    int valid = 0;
    for (int si = 0; si < kOrdStates; ++si)
        for (int ei = 0; ei < kOrdEvents; ++ei)
            for (bool filled : {false, true}) {
                S s = S(si);
                E e = E(ei);
                std::optional<S> want;
                for (const Rule& r : kSpec)
                    if (r.from == s && r.event == e) want = filled ? r.to_filled : r.to_unfilled;
                if (is_terminal(s) && (e == E::CancelRejected || e == E::ReplaceRejected)) want = s;
                std::optional<S> got = transition(s, e, filled);
                assert(got == want);
                valid += got.has_value();
            }
    assert(valid == 2 * (int(kSpec.size()) + 8));  // 8 = 4 terminal states x 2 late rejects
}

static void test_terminal_states_accept_nothing_else() {
    for (S s : {S::Filled, S::Cancelled, S::Rejected, S::Expired}) {
        assert(is_terminal(s));
        for (E e : {E::CancelReq, E::ReplaceReq, E::Ack, E::PartialFill, E::Fill, E::Cancelled})
            assert(!transition(s, e, true));
    }
}

// OMS wired straight to a real Venue. Requests queue in `req` until send(),
// which lets tests hold a request back and create races on purpose.
struct Rig {
    Venue v;
    SymbolId s = v.add_symbol();
    Oms oms{7, 1'000'000};
    std::vector<VenueRequest> req;
    VenueOut out;

    void send() {
        for (const VenueRequest& r : req) v.on_request(r, out);
        req.clear();
        deliver();
    }
    void deliver() {
        for (const ExecReport& rep : out.reports) oms.on_report(rep);
        out.clear();
    }
    // Other market participants: ids below the OMS range, anonymous account.
    void other(OrderId id, Side side, Price px, Qty qty, Tif tif = Tif::Day) {
        v.on_new({id, 0, s, side, OrdType::Limit, tif, px, qty}, out);  // account 0: no STP
        deliver();
    }
    const OmsOrder& o(ClOrdId cl) const { return *oms.find(cl); }
    ClientOrder lim(ClOrdId cl, Side side, Price px, Qty qty, Tif tif = Tif::Day) const {
        return {cl, s, side, OrdType::Limit, tif, px, qty};
    }
};

static void test_lifecycle_new_partial_full() {
    Rig r;
    r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);
    assert(r.o(1).state == S::PendingNew && r.req.size() == 1);
    r.send();
    assert(r.o(1).state == S::Acknowledged && r.o(1).leaves == 10);

    r.oms.set_time(500);
    r.other(1, Side::Sell, 100, 4);
    assert(r.o(1).state == S::PartiallyFilled && r.o(1).cum == 4 && r.o(1).leaves == 6);
    r.other(2, Side::Sell, 99, 6);  // prints at our resting 100
    assert(r.o(1).state == S::Filled && r.o(1).cum == 10 && r.o(1).leaves == 0);
    assert(r.o(1).avg_px() == 100.0);

    const auto& f = r.oms.fills();
    assert(f.size() == 2 && f[0].qty == 4 && f[1].qty == 6 && f[0].time == 500 && f[0].cl == 1);
    assert(r.oms.invalid_transitions() == 0 && r.oms.foreign_reports() == 4);  // New + aggressor Trade, x2
}

static void test_duplicate_submit_is_idempotent() {
    Rig r;
    auto a = r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);
    auto b = r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);  // retry after a "timeout"
    assert(!a.duplicate && b.duplicate && &a.order == &b.order);
    assert(r.req.size() == 1 && r.oms.order_count() == 1);
    r.send();
    assert(r.v.book(r.s).order_count() == 1);
}

static void test_cancel_and_local_refusals() {
    Rig r;
    r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);
    r.send();
    assert(r.oms.cancel(1, r.req) && r.o(1).state == S::CancelPending);
    assert(!r.oms.cancel(1, r.req) && !r.oms.replace(1, 99, 5, r.req));  // one request in flight
    r.send();
    assert(r.o(1).state == S::Cancelled && r.o(1).leaves == 0);
    assert(!r.oms.cancel(1, r.req) && !r.oms.cancel(42, r.req) && r.req.empty());
}

static void test_replace_and_replace_reject_restores() {
    Rig r;
    r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);
    r.send();
    assert(r.oms.replace(1, 101, 5, r.req) && r.o(1).state == S::ReplacePending);
    r.send();
    assert(r.o(1).state == S::Acknowledged && r.o(1).px == 101 && r.o(1).leaves == 5);

    r.other(1, Side::Sell, 101, 2);
    assert(r.o(1).state == S::PartiallyFilled);
    r.oms.replace(1, 101 + OrderBook::kMaxSpan, 5, r.req);  // venue refuses: out of band
    r.send();
    assert(r.o(1).state == S::PartiallyFilled && r.o(1).px == 101 && r.o(1).leaves == 3);
    assert(r.o(1).last_reject == Reject::PriceBand);
}

static void test_ioc_reject_and_stop_paths() {
    Rig r;
    r.other(1, Side::Sell, 100, 3);
    r.oms.submit(r.lim(1, Side::Buy, 100, 5, Tif::Ioc), r.req);
    r.oms.submit(r.lim(2, Side::Buy, 100, 0), r.req);
    r.send();
    assert(r.o(1).state == S::Cancelled && r.o(1).cum == 3 && r.o(1).leaves == 0);
    assert(r.o(2).state == S::Rejected && r.o(2).last_reject == Reject::ZeroQty);

    r.oms.submit({3, r.s, Side::Buy, OrdType::StopLimit, Tif::Day, 105, 2, 101}, r.req);
    r.send();
    assert(r.o(3).state == S::Acknowledged);
    r.other(2, Side::Sell, 101, 1);
    r.other(3, Side::Buy, 101, 1);  // prints 101: stop fires, rests at 105
    r.other(4, Side::Sell, 104, 2);
    assert(r.o(3).state == S::Filled && r.o(3).avg_px() == 105.0);
    assert(r.oms.invalid_transitions() == 0);
}

static void test_invalid_venue_message_is_counted_not_applied() {
    Rig r;
    r.oms.submit(r.lim(1, Side::Buy, 100, 10), r.req);
    r.send();
    OrderId id = r.o(1).id;
    r.oms.on_report({id, r.s, ExecType::Replaced, Reject::None, 0, 0, 99});  // no replace was asked
    assert(r.oms.invalid_transitions() == 1 && r.o(1).state == S::Acknowledged && r.o(1).leaves == 10);
}

static_assert(transition(OrdState::CancelPending, OrdEvent::Fill, true) == OrdState::Filled,
              "a fill in flight beats the cancel");

int main() {
    test_every_state_event_pair_matches_spec();
    test_terminal_states_accept_nothing_else();
    test_lifecycle_new_partial_full();
    test_duplicate_submit_is_idempotent();
    test_cancel_and_local_refusals();
    test_replace_and_replace_reject_restores();
    test_ioc_reject_and_stop_paths();
    test_invalid_venue_message_is_counted_not_applied();
    std::puts("all tests passed");
}
