#pragma once
#include <cassert>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

#include "core/types.hpp"

// Deterministic discrete-event loop on a simulated clock. Events fire in
// (time, schedule order), so equal-time events keep FIFO order and the same
// inputs always produce the same run. Network/venue latency is modeled by
// scheduling an event later. Event is a small copyable value (e.g. a
// std::variant of message structs); the caller dispatches it.
template <class Event>
class EventLoop {
public:
    explicit EventLoop(size_t reserve = 1024) {
        std::vector<Item> v;
        v.reserve(reserve);
        q_ = Queue(Later{}, std::move(v));
    }

    Nanos now() const { return now_; }
    bool empty() const { return q_.empty(); }
    size_t pending() const { return q_.size(); }

    // Events cannot fire in the past: asserts in debug, clamps to now in release.
    void schedule_at(Nanos t, const Event& e) {
        assert(t >= now_);
        q_.push({t < now_ ? now_ : t, seq_++, e});
    }
    void schedule_in(Nanos delay, const Event& e) { schedule_at(now_ + delay, e); }

    // Fires every event with time <= until, in order, then sets the clock to
    // `until`. The handler may schedule more events, including at now().
    // Returns the number of events fired.
    template <class Handler>
    size_t run_until(Nanos until, Handler&& handle) {
        size_t fired = drain(until, handle);
        if (until > now_) now_ = until;
        return fired;
    }

    // Fires events until none are left; the clock stays at the last event.
    template <class Handler>
    size_t run(Handler&& handle) {
        return drain(std::numeric_limits<Nanos>::max(), handle);
    }

private:
    template <class Handler>
    size_t drain(Nanos until, Handler& handle) {
        size_t fired = 0;
        while (!q_.empty() && q_.top().t <= until) {
            Item it = q_.top();
            q_.pop();
            now_ = it.t;
            handle(it.e);
            ++fired;
        }
        return fired;
    }

    struct Item {
        Nanos t;
        uint64_t seq;
        Event e;
    };
    struct Later {
        bool operator()(const Item& a, const Item& b) const {
            return a.t != b.t ? a.t > b.t : a.seq > b.seq;
        }
    };
    using Queue = std::priority_queue<Item, std::vector<Item>, Later>;

    Queue q_;
    Nanos now_ = 0;
    uint64_t seq_ = 0;
};
