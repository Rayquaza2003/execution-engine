#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "oms/oms.hpp"
#include "risk/risk.hpp"
#include "web/http.hpp"

// The engine behind the test bench: a venue with one book per symbol, your
// orders through the OMS and pre-trade risk, a second account that trades
// straight at the venue (to cross against yourself and see self-trade
// prevention), and a background flow of makers and takers so the books are
// alive. Every request to the venue is timed. Single-threaded: the HTTP loop
// calls in, and state() returns a JSON snapshot for the page.
class Sandbox {
public:
    static constexpr AccountId kMe = 1, kMaker = 2, kOther = 3, kTaker = 4;
    static constexpr OrderId kMeBase = 1'000'000'000, kOtherBase = 2'000'000'000;

    using Form = std::unordered_map<std::string, std::string>;

    Sandbox(std::vector<std::string> symbols, std::vector<Price> mids, RiskLimits limits)
        : names_(std::move(symbols)), start_mids_(std::move(mids)), limits_(limits) {
        reset();
    }

    // A fresh session: empty books (then seeded by the flow), no orders, no position.
    void reset() {
        venue_ = std::make_unique<Venue>();
        risk_ = std::make_unique<Risk>(names_.size(), limits_);
        oms_ = std::make_unique<Oms>(kMe, kMeBase, risk_.get());
        bots_.assign(names_.size(), {});
        for (size_t s = 0; s < names_.size(); ++s) {
            venue_->add_symbol();
            bots_[s].mid = start_mids_[s];
        }
        tape_.clear();
        events_.clear();
        lat_ = Histogram();
        ops_ = 0;
        next_cl_ = 1;
        next_flow_ = 1;
        next_other_ = kOtherBase;
        burst_ = "";
        for (size_t i = 0; i < 200 * names_.size(); ++i) flow_event(SymbolId(i % names_.size()), true);  // starting liquidity
    }

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
            events_.push_front({now / 1'000'000, o.cl, "me", "Rejected", reject_name(o.last_reject), 0, 0, 0});
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

    void set_flow(double per_second) { flow_rate_ = std::clamp(per_second, 0.0, 200'000.0); }

    // n background orders as fast as the engine takes them.
    std::string burst(long long n, Nanos now) {
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

    // Background flow for the time since the last call.
    void tick(Nanos now) {
        if (!last_tick_) last_tick_ = now;
        double due = flow_rate_ * double(now - last_tick_) / 1e9 + carry_;
        last_tick_ = now;
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
        Price center = bid && ask ? (*bid + *ask) / 2 : bid ? *bid : ask ? *ask : bots_[focus].mid;
        Price hi = center + 14, lo = center - 14;
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
             ",\"burst\":" + web::quote(burst_) + ",\"now\":" + std::to_string(now / 1'000'000) + "}}";
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
        else risk_->mark(s, bots_[s].mid);
    }

    void send(const VenueRequest& rq, Nanos now) {
        venue_->set_time(now);
        auto t0 = std::chrono::steady_clock::now();
        venue_->on_request(rq, out_);
        lat_.record(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
        ++ops_;
        for (const TradePrint& p : out_.prints) {
            tape_.push_front({now / 1'000'000, p.sym, p.price, p.qty, p.aggressor == Side::Buy});
            if (tape_.size() > 400) tape_.pop_back();
        }
        for (const ExecReport& r : out_.reports) {
            if (r.id >= kMeBase && r.id < kOtherBase) {
                oms_->set_time(now);
                oms_->on_report(r);
                if (const OmsOrder* o = oms_->by_id(r.id))
                    events_.push_front({now / 1'000'000, o->cl, "me", exec_name(r.type), reject_name(r.reason), r.last_px,
                                        r.last_qty, r.leaves});
            } else if (r.id >= kOtherBase) {
                events_.push_front({now / 1'000'000, r.id - kOtherBase + 1, "other", exec_name(r.type), reject_name(r.reason),
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

    std::vector<std::string> names_;
    std::vector<Price> start_mids_;
    RiskLimits limits_;
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
