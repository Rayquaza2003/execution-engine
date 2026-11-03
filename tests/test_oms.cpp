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

static_assert(transition(OrdState::CancelPending, OrdEvent::Fill, true) == OrdState::Filled,
              "a fill in flight beats the cancel");

int main() {
    test_every_state_event_pair_matches_spec();
    test_terminal_states_accept_nothing_else();
    std::puts("all tests passed");
}
