#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/histogram.hpp"
#include "exchange/venue.hpp"
#include "md/replay.hpp"
#include "oms/oms.hpp"
#include "risk/risk.hpp"
#include "web/http.hpp"

// The engine behind the test bench: a venue with one book per symbol, your
// orders through the OMS and pre-trade risk, and a second account that trades
// straight at the venue (to cross against yourself and see self-trade
// prevention). The market around you comes from one of two places:
//
//   simulated: a background flow of makers and takers around a drifting mid
//   replay:    a NASDAQ ITCH file, played back at a chosen speed into the
//              same books. Your orders rest among the real ones and fill when
//              real executions reach them in the queue (Venue::replay_execute).
//
// Every request to the venue is timed. Single-threaded: the HTTP loop calls
// in, and state() returns a JSON snapshot for the page.

// An ITCH file already in memory (mmap) and the symbols to rebuild.
struct ReplayFeed {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    std::vector<std::string> symbols;
    Nanos start = 34'200'000'000'000;  // 09:30: everything before is fast-forwarded on reset
};

class Sandbox {
public:
    static constexpr AccountId kMe = 1, kMaker = 2, kOther = 3, kTaker = 4;
    static constexpr OrderId kMeBase = 1'000'000'000, kOtherBase = 2'000'000'000;

    using Form = std::unordered_map<std::string, std::string>;

    Sandbox(std::vector<std::string> symbols, std::vector<Price> mids, RiskLimits limits, ReplayFeed feed = {})
        : sim_names_(std::move(symbols)), start_mids_(std::move(mids)), limits_(limits), feed_(std::move(feed)) {
        reset();
    }

    bool replay_available() const { return feed_.data != nullptr; }

    // A fresh session in the current mode: no orders, no position. Simulated
    // books are seeded by the flow; replayed books are rebuilt from the file
    // up to its start time (09:30), and playback waits for play().
    void reset() {
        venue_ = std::make_unique<Venue>();
        tape_.clear();
        events_.clear();
        lat_ = Histogram();
        ops_ = 0;
        next_cl_ = 1;
        next_flow_ = 1;
        next_other_ = kOtherBase;
        burst_ = "";
        bots_.clear();
        names_.clear();
        replay_.reset();
        if (replay_mode_) {
            replay_ = std::make_unique<ItchReplay>(*venue_, feed_.symbols);
            offset_ = 0;
            clock_ = 0;
            feed_msgs_ = 0;
            playing_ = false;
            at_end_ = false;
            advance_to(feed_.start, ~size_t(0));  // pre-market, at full speed
            for (SymbolId i = 0; i < venue_->symbols(); ++i) names_.push_back(replay_->name(i));
        } else {
            names_ = sim_names_;
            bots_.assign(names_.size(), {});
            for (size_t i = 0; i < names_.size(); ++i) {
                venue_->add_symbol();
                bots_[i].mid = start_mids_[i];
            }
        }
        risk_ = std::make_unique<Risk>(std::max<size_t>(names_.size(), 1), limits_);
        oms_ = std::make_unique<Oms>(kMe, kMeBase, risk_.get());
        if (!replay_mode_)
            for (size_t i = 0; i < 200 * names_.size(); ++i) flow_event(SymbolId(i % names_.size()), true);
    }

    // Switch market source; starts a fresh session.
    std::string set_mode(bool replay) {
        if (replay && !feed_.data) return "No ITCH file loaded. Start web_ui with --itch FILE SYMBOL...";
        replay_mode_ = replay;
        reset();
        return "";
    }

    // Replay controls. speed: replayed seconds per real second; 0 = as fast as possible.
    std::string play(bool on) {
        if (!replay_mode_) return "Replay is off.";
        if (on && at_end_) return "The file has no more messages. Reset to start again at 09:30.";
        playing_ = on;
        return "";
    }
    void set_speed(double x) { speed_ = std::clamp(x, 0.0, 100'000.0); }

    // POST /api/order: sym, side (buy|sell), type (limit|market|stop), tif
    // (day|gtc|ioc|fok), px and stop in dollars, qty, who (me|other).
    std::string order(const Form& f, Nanos now) {
        std::optional<SymbolId> s = symbol(get(f, "sym"));
        if (!s) return "Pick a symbol.";
        Side side = get(f, "side") == "sell" ? Side::Sell : Side::Buy;
        std::string type = get(f, "type");
        OrdType ot = type == "market" ? OrdType::Market : type == "stop" ? OrdType::StopLimit : OrdType::Limit;
        std::string t = get(f, "tif");
        Tif tif = t == "gtc" ? Tif::Gtc : t == "ioc" ? Tif::Ioc : t == "fok" ? Tif::Fok : Tif::Day;
        if (ot == OrdType::Market) tif = Tif::Ioc;
        long long qty = std::atoll(get(f, "qty").c_str());
        if (qty <= 0 || qty > 1'000'000) return "Quantity must be between 1 and 1,000,000.";
        std::optional<Price> px = ticks(get(f, "px")), stop = ticks(get(f, "stop"));
        if (ot != OrdType::Market && (!px || *px <= 0)) return "Enter a limit price above zero.";
        if (ot == OrdType::StopLimit && (!stop || *stop <= 0)) return "Enter a stop price above zero.";
        mark(*s);
        if (get(f, "who") == "other") {
            send(NewOrder{next_other_++, kOther, *s, side, ot, tif, px.value_or(0), Qty(qty), stop.value_or(0)}, now);
            return "";
        }
        std::vector<VenueRequest> reqs;
        const OmsOrder& o =
            oms_->submit({next_cl_++, *s, side, ot, tif, px.value_or(0), Qty(qty), stop.value_or(0)}, reqs).order;
        if (o.state == OrdState::Rejected) {
            events_.push_front({tod_ms(now), o.cl, "me", "Rejected", reject_name(o.last_reject), 0, 0, 0});
            return std::string("Risk refused it: ") + reject_name(o.last_reject) + ".";
        }
        for (const VenueRequest& rq : reqs) send(rq, now);
        return "";
    }

    std::string cancel(const Form& f, Nanos now) {
        std::vector<VenueRequest> reqs;
        if (!oms_->cancel(ClOrdId(std::atoll(get(f, "cl").c_str())), reqs)) return "That order can't be cancelled now.";
        for (const VenueRequest& rq : reqs) send(rq, now);
        return "";
    }

    std::string replace(const Form& f, Nanos now) {
        ClOrdId cl = ClOrdId(std::atoll(get(f, "cl").c_str()));
        const OmsOrder* o = oms_->find(cl);
        std::optional<Price> px = ticks(get(f, "px"));
        long long qty = std::atoll(get(f, "qty").c_str());
        if (!o || !px || *px <= 0 || qty <= 0) return "Enter a new price and quantity.";
        mark(o->sym);
        std::vector<VenueRequest> reqs;
        if (!oms_->replace(cl, *px, Qty(qty), reqs))
            return o->last_reject != Reject::None ? std::string("Risk refused it: ") + reject_name(o->last_reject) + "."
                                                  : "That order can't be modified now.";
        for (const VenueRequest& rq : reqs) send(rq, now);
        return "";
    }

    // Kill switch: risk refuses everything from now on, and every working order is cancelled.
    std::string kill(Nanos now) {
        std::vector<VenueRequest> reqs;
        oms_->kill(reqs);
        for (const VenueRequest& rq : reqs) send(rq, now);
        return "";
    }

    // Trading allowed again after a kill; what the kill cancelled stays cancelled.
    std::string resume() {
        risk_->resume();
        return "";
    }

    std::string set_flow(double per_second) {
        if (replay_mode_) return "No simulated flow during replay: the real market is the flow.";
        flow_rate_ = std::clamp(per_second, 0.0, 200'000.0);
        return "";
    }

    // n background orders as fast as the engine takes them.
    std::string burst(long long n, Nanos now) {
        if (replay_mode_) return "No simulated flow during replay: the real market is the flow.";
        n = std::clamp(n, 1LL, 2'000'000LL);
        auto t0 = std::chrono::steady_clock::now();
        for (long long i = 0; i < n; ++i) flow_event(SymbolId(i % names_.size()), false, now);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char b[160];
        std::snprintf(b, sizeof b, "%lld orders in %.1f ms: %.2f M orders/s, through the HTTP loop's thread", n,
                      secs * 1e3, double(n) / secs / 1e6);
        burst_ = b;
        return "";
    }

    // The market moves on: replay plays the file forward by elapsed time x
    // speed; the simulation runs its background flow.
    void tick(Nanos now) {
        if (!last_tick_) last_tick_ = now;
        Nanos dt = now - last_tick_;
        last_tick_ = now;
        if (replay_mode_) {
            if (!playing_) return;
            Nanos target = speed_ > 0 ? clock_ + Nanos(double(dt) * speed_) : ~Nanos(0);
            advance_to(target, kMaxPerTick);
            if (at_end_) playing_ = false;
            return;
        }
        double due = flow_rate_ * double(dt) / 1e9 + carry_;
        int n = int(std::min(due, 5000.0));
        carry_ = due - n;
        for (int i = 0; i < n; ++i) flow_event(SymbolId(rng_() % names_.size()), false, now);
    }

    // Everything the page shows, for symbol `focus`.
    std::string state(SymbolId focus, Nanos now) const {
        if (focus >= names_.size()) focus = 0;
        const OrderBook& b = venue_->book(focus);
        std::string j = "{\"symbols\":[";
        for (size_t i = 0; i < names_.size(); ++i) j += (i ? "," : "") + web::quote(names_[i]);
        j += "],\"sym\":" + std::to_string(focus);
        // Ladder: a fixed window of prices around the touch, with our resting quantity per price.
        std::unordered_map<Price, Qty> mine_b, mine_s;
        for (size_t i = 0; i < oms_->order_count(); ++i) {
            const OmsOrder& o = *oms_->by_id(kMeBase + i);
            if (o.sym == focus && !is_terminal(o.state) && o.type != OrdType::Market)
                (o.side == Side::Buy ? mine_b : mine_s)[o.px] += o.leaves;
        }
        std::optional<Price> bid = b.best_bid(), ask = b.best_ask();
        Price center = bid && ask ? (*bid + *ask) / 2 : bid ? *bid : ask ? *ask : bots_.empty() ? 0 : bots_[focus].mid;
        // 29 rows around the mid, wider when the spread is (a few levels past each touch), at most 81.
        Price hi = center + 14, lo = center - 14;
        if (ask) hi = std::max(hi, *ask + 6);
        if (bid) lo = std::min(lo, *bid - 6);
        if (hi - lo > 80) hi = center + 40, lo = center - 40;
        j += ",\"bid\":" + num(bid ? double(*bid) / 100 : NAN) + ",\"ask\":" + num(ask ? double(*ask) / 100 : NAN);
        j += ",\"ladder\":[";
        for (Price p = hi; p >= lo; --p) {
            j += p == hi ? "[" : ",[";
            j += num(double(p) / 100) + "," + std::to_string(b.depth(Side::Buy, p)) + "," +
                 std::to_string(b.depth(Side::Sell, p)) + "," + std::to_string(mine_b.count(p) ? mine_b.at(p) : 0) +
                 "," + std::to_string(mine_s.count(p) ? mine_s.at(p) : 0) + "]";
        }
        j += "],\"tape\":[";
        size_t k = 0;
        for (const Print& p : tape_) {
            if (p.sym != focus) continue;
            if (k++) j += ",";
            j += "[" + std::to_string(p.ms) + "," + num(double(p.px) / 100) + "," + std::to_string(p.qty) + "," +
                 (p.buy ? "1" : "0") + "]";
            if (k == 40) break;
        }
        j += "],\"orders\":[";
        size_t shown = 0;
        for (size_t i = oms_->order_count(); i-- > 0 && shown < 60; ++shown) {
            const OmsOrder& o = *oms_->by_id(kMeBase + i);
            Qty ahead = 0;
            bool resting = !is_terminal(o.state) && o.type == OrdType::Limit && o.leaves;
            if (resting) {
                bool found = false;
                venue_->book(o.sym).queue(o.side, o.px, [&](OrderId id, Qty q) {
                    if (id == o.id) found = true;
                    if (!found) ahead += q;
                });
                if (!found) ahead = 0;
            }
            if (shown) j += ",";
            j += "{\"cl\":" + std::to_string(o.cl) + ",\"sym\":" + web::quote(names_[o.sym]) +
                 ",\"side\":" + (o.side == Side::Buy ? "\"buy\"" : "\"sell\"") +
                 ",\"type\":" + web::quote(o.type == OrdType::Market ? "market" : o.type == OrdType::StopLimit ? "stop" : "limit") +
                 ",\"tif\":" + web::quote(tif_name(o.tif)) + ",\"px\":" + num(double(o.px) / 100) +
                 ",\"qty\":" + std::to_string(o.cum + o.leaves) + ",\"cum\":" + std::to_string(o.cum) +
                 ",\"leaves\":" + std::to_string(o.leaves) + ",\"avg\":" + num(o.cum ? o.avg_px() / 100 : NAN) +
                 ",\"state\":" + web::quote(state_name(o.state)) + ",\"reject\":" + web::quote(o.last_reject == Reject::None ? "" : reject_name(o.last_reject)) +
                 ",\"ahead\":" + (resting ? std::to_string(ahead) : "null") + "}";
        }
        j += "],\"events\":[";
        k = 0;
        for (const Event& e : events_) {
            if (k++) j += ",";
            j += "[" + std::to_string(e.ms) + "," + std::to_string(e.cl) + "," + web::quote(e.who) + "," +
                 web::quote(e.what) + "," + web::quote(e.why) + "," + num(double(e.px) / 100) + "," +
                 std::to_string(e.qty) + "," + std::to_string(e.leaves) + "]";
            if (k == 40) break;
        }
        j += "],\"risk\":{\"position\":" + std::to_string(risk_->position(focus)) +
             ",\"open_buy\":" + std::to_string(risk_->open_qty(focus, Side::Buy)) +
             ",\"open_sell\":" + std::to_string(risk_->open_qty(focus, Side::Sell)) +
             ",\"open_orders\":" + std::to_string(risk_->open_orders()) + ",\"pnl\":" + num(risk_->pnl() / 100) +
             ",\"killed\":" + (risk_->killed() ? "true" : "false") +
             ",\"max_position\":" + std::to_string(limits_.max_position) +
             ",\"max_order_qty\":" + std::to_string(limits_.max_order_qty) +
             ",\"max_open_orders\":" + std::to_string(limits_.max_open_orders) +
             ",\"collar_pct\":" + num(limits_.collar_bps / 100.0) + ",\"max_loss\":" + num(limits_.max_loss / 100) + "}";
        j += ",\"stats\":{\"requests\":" + std::to_string(ops_) + ",\"p50\":" + std::to_string(lat_.percentile(0.5)) +
             ",\"p99\":" + std::to_string(lat_.percentile(0.99)) + ",\"p999\":" + std::to_string(lat_.percentile(0.999)) +
             ",\"resting\":" + std::to_string(b.order_count()) + ",\"flow\":" + num(flow_rate_) +
             ",\"burst\":" + web::quote(burst_) + ",\"now\":" + std::to_string(tod_ms(now)) + "}";
        j += ",\"mode\":\"" + std::string(replay_mode_ ? "replay" : "sim") + "\"";
        j += ",\"replay\":{\"available\":" + std::string(feed_.data ? "true" : "false") +
             ",\"playing\":" + (playing_ ? "true" : "false") + ",\"end\":" + (at_end_ ? "true" : "false") +
             ",\"speed\":" + num(speed_) + ",\"clock\":" + std::to_string(clock_ / 1'000'000) +
             ",\"messages\":" + std::to_string(feed_msgs_) +
             ",\"progress\":" + num(feed_.bytes ? 100.0 * double(offset_) / double(feed_.bytes) : 0) + "}}";
        return j;
    }

    std::optional<SymbolId> symbol(const std::string& name) const {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name || std::to_string(i) == name) return SymbolId(i);
        return std::nullopt;
    }

private:
    struct Bot {
        Price mid = 0;
        std::deque<OrderId> live;  // maker orders that may still rest
    };
    struct Print {
        uint64_t ms;
        SymbolId sym;
        Price px;
        Qty qty;
        bool buy;  // aggressor
    };
    struct Event {
        uint64_t ms;
        ClOrdId cl;
        std::string who, what, why;
        Price px;
        Qty qty, leaves;
    };

    static std::string get(const Form& f, const char* k) {
        auto it = f.find(k);
        return it == f.end() ? "" : it->second;
    }
    // "150.25" -> 15025 ticks (cents).
    static std::optional<Price> ticks(const std::string& s) {
        if (s.empty()) return std::nullopt;
        char* end;
        double d = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || !std::isfinite(d) || std::fabs(d) > 1e7) return std::nullopt;
        return Price(std::llround(d * 100));
    }
    static std::string num(double v) {
        if (std::isnan(v)) return "null";
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", v);
        return b;
    }
    static const char* tif_name(Tif t) {
        constexpr const char* n[] = {"day", "gtc", "ioc", "fok"};
        return n[int(t)];
    }
    static const char* state_name(OrdState s) {
        constexpr const char* n[] = {"sent", "working", "partly filled", "filled", "cancelling",
                                     "modifying", "cancelled", "rejected", "expired"};
        return n[int(s)];
    }
    static const char* exec_name(ExecType t) {
        constexpr const char* n[] = {"Accepted", "Filled", "Cancelled", "Modified", "Rejected", "Expired",
                                     "Cancel refused", "Modify refused", "Stop triggered"};
        return n[int(t)];
    }
    static const char* reject_name(Reject r) {
        constexpr const char* n[] = {"",
                                     "zero quantity",
                                     "duplicate id",
                                     "price outside the band",
                                     "unknown order",
                                     "unknown symbol",
                                     "symbol halted",
                                     "self-trade prevented",
                                     "kill switch is on",
                                     "order too large",
                                     "notional too large",
                                     "position limit",
                                     "too many open orders",
                                     "price too far from the market",
                                     "loss limit reached",
                                     "no market price yet"};
        return n[int(r)];
    }

    // Risk needs a reference price for its checks: the current mid.
    void mark(SymbolId s) {
        const OrderBook& b = venue_->book(s);
        if (b.best_bid() && b.best_ask()) risk_->mark(s, (*b.best_bid() + *b.best_ask()) / 2);
        else if (b.best_bid() || b.best_ask()) risk_->mark(s, b.best_bid().value_or(b.best_ask().value_or(0)));
        else if (!bots_.empty()) risk_->mark(s, bots_[s].mid);
        // replay with an empty book: no mark, so risk refuses with "no market price yet"
    }

    // What the page's clocks show, as ms since midnight: the ITCH clock in
    // replay (New York time), the local wall clock in the simulation.
    uint64_t tod_ms(Nanos now) const {
        if (replay_mode_) return clock_ / 1'000'000;
        std::time_t t = std::time_t(now / 1'000'000'000);
        std::tm tm;
        localtime_r(&t, &tm);
        return uint64_t((tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) * 1000) + now / 1'000'000 % 1000;
    }

    // Plays the file forward to `target` (ITCH ns since midnight), at most
    // `max_msgs` messages. Feed orders rest in the same books as ours.
    void advance_to(Nanos target, size_t max_msgs) {
        size_t n = 0;
        Nanos last = clock_;
        while (offset_ + 2 <= feed_.bytes) {
            size_t len = itch::be<2>(feed_.data + offset_);
            if (offset_ + 2 + len > feed_.bytes) break;  // a cut-off last message
            const uint8_t* m = feed_.data + offset_ + 2;
            if (len >= 11) {
                Nanos ts = itch::be<6>(m + 5);
                if (ts > target) {
                    clock_ = std::max(clock_, target);
                    return;
                }
                last = std::max(last, ts);
            }
            if (n++ == max_msgs) {  // done for this tick
                clock_ = last;
                return;
            }
            itch::parse(feed_.data + offset_, 2 + len, [&](const auto& msg) { replay_->apply(msg, out_); });
            offset_ += 2 + len;
            ++feed_msgs_;
            if (!out_.prints.empty() || !out_.reports.empty()) route(last);
            else out_.clear();
        }
        at_end_ = true;
        clock_ = last;
    }

    void send(const VenueRequest& rq, Nanos now) {
        venue_->set_time(replay_mode_ ? clock_ : now);
        auto t0 = std::chrono::steady_clock::now();
        venue_->on_request(rq, out_);
        lat_.record(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
        ++ops_;
        route(now);
    }

    // Venue output: prints to the tape, reports to their owner (the OMS for
    // ours, the event list for the other trader's). Feed orders have no owner.
    void route(Nanos now) {
        uint64_t ms = tod_ms(now);
        for (const TradePrint& p : out_.prints) {
            tape_.push_front({ms, p.sym, p.price, p.qty, p.aggressor == Side::Buy});
            if (tape_.size() > 400) tape_.pop_back();
        }
        for (const ExecReport& r : out_.reports) {
            if (r.id >= kMeBase && r.id < kOtherBase) {
                oms_->set_time(now);
                oms_->on_report(r);
                if (const OmsOrder* o = oms_->by_id(r.id))
                    events_.push_front({ms, o->cl, "me", exec_name(r.type), reject_name(r.reason), r.last_px, r.last_qty,
                                        r.leaves});
            } else if (r.id >= kOtherBase && r.id < ItchReplay::kIdBase) {
                events_.push_front({ms, r.id - kOtherBase + 1, "other", exec_name(r.type), reject_name(r.reason),
                                    r.last_px, r.last_qty, r.leaves});
            }
            if (events_.size() > 200) events_.pop_back();
        }
        out_.clear();
    }

    // One background order: makers rest near the mid, takers hit it, the mid drifts.
    void flow_event(SymbolId s, bool seeding, Nanos now = 0) {
        Bot& bot = bots_[s];
        int k = int(rng_() % 100);
        if (rng_() % 40 == 0) bot.mid += rng_() % 2 ? 1 : -1;
        if (seeding || k < 55) {
            Side side = rng_() % 2 ? Side::Buy : Side::Sell;
            Price off = Price(1 + rng_() % 10);
            Price px = side == Side::Buy ? bot.mid - off : bot.mid + off;
            OrderId id = next_flow_++;
            send(NewOrder{id, kMaker, s, side, OrdType::Limit, Tif::Day, px, Qty(100 * (1 + rng_() % 5))}, now);
            bot.live.push_back(id);
            if (bot.live.size() > 3000) {  // cancel the oldest rather than forget it, or it rests forever
                send(CancelRequest{bot.live.front(), s}, now);
                bot.live.pop_front();
            }
        } else if (k < 88 && !bot.live.empty()) {
            size_t i = rng_() % bot.live.size();
            send(CancelRequest{bot.live[i], s}, now);
            bot.live[i] = bot.live.back();
            bot.live.pop_back();
        } else {
            send(NewOrder{next_flow_++, kTaker, s, rng_() % 2 ? Side::Buy : Side::Sell, OrdType::Market, Tif::Ioc, 0,
                          Qty(100 * (1 + rng_() % 3))},
                 now);
        }
    }

    static constexpr size_t kMaxPerTick = 300'000;  // feed messages per tick at full speed

    std::vector<std::string> sim_names_, names_;  // names_: the session's symbols, by SymbolId
    std::vector<Price> start_mids_;
    RiskLimits limits_;
    ReplayFeed feed_;
    bool replay_mode_ = false;
    std::unique_ptr<ItchReplay> replay_;
    size_t offset_ = 0;      // next byte of the file
    Nanos clock_ = 0;        // replay time, ITCH ns since midnight
    uint64_t feed_msgs_ = 0;
    bool playing_ = false, at_end_ = false;
    double speed_ = 1;
    std::unique_ptr<Venue> venue_;
    std::unique_ptr<Risk> risk_;
    std::unique_ptr<Oms> oms_;
    std::vector<Bot> bots_;
    std::deque<Print> tape_;
    std::deque<Event> events_;
    VenueOut out_;
    Histogram lat_;
    uint64_t ops_ = 0;
    ClOrdId next_cl_ = 1;
    OrderId next_flow_ = 1, next_other_ = kOtherBase;
    double flow_rate_ = 200, carry_ = 0;
    Nanos last_tick_ = 0;
    std::mt19937_64 rng_{42};
    std::string burst_;
};
