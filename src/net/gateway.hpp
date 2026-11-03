#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/histogram.hpp"
#include "core/spsc.hpp"
#include "exchange/venue.hpp"
#include "fix/session.hpp"

// FIX order entry for the venue. One thread runs a poll() loop over the
// listening socket and every client connection: it frames and sequences FIX
// (Session), turns NewOrderSingle (D), OrderCancelRequest (F) and
// OrderCancelReplaceRequest (G) into venue requests, and turns the venue's
// execution reports back into ExecutionReport (8) and OrderCancelReject (9).
// The matching engine runs on its own thread behind two SPSC queues, so a
// slow client never stalls matching.
//
// Both threads busy-poll: lowest latency, one core each.
// ponytail: no cancel-on-disconnect: a client's resting orders outlive its
// connection; add it when a client asks for it.
class Gateway {
public:
    Gateway(std::vector<std::string> symbols, uint16_t port = 0, std::string comp_id = "EXCH")
        : symbols_(std::move(symbols)), comp_(std::move(comp_id)) {
        listen_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_, 16) != 0) {
            std::perror("gateway bind/listen");
            std::abort();
        }
        socklen_t len = sizeof a;
        getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        nonblocking(listen_);
    }
    ~Gateway() {
        for (auto& c : conns_)
            if (c && c->fd >= 0) close(c->fd);
        close(listen_);
    }
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    uint16_t port() const { return port_; }

    // Serves until stop is set.
    void run(const std::atomic<bool>& stop) {
        std::thread matcher([&] { match(stop); });
        std::vector<pollfd> fds;
        char buf[1 << 16];
        Nanos next_timer = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fds.assign(1, {listen_, POLLIN, 0});
            for (auto& c : conns_) fds.push_back({c ? c->fd : -1, short(c && !c->s.output().empty() ? POLLIN | POLLOUT : POLLIN), 0});
            if (poll(fds.data(), nfds_t(fds.size()), 0) < 0) continue;
            Nanos t = now();
            if (fds[0].revents & POLLIN) accept_all(t);
            for (size_t i = 1; i < fds.size(); ++i) {
                Conn* c = conns_[i - 1].get();
                if (!c || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t n = read(c->fd, buf, sizeof buf);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) drop(i - 1);
                    continue;
                }
                c->s.on_bytes(buf, size_t(n), t, [&](const fix::Message& m) { on_app(uint32_t(i - 1), m, t); });
            }
            // Every request gets an answer from the matcher within a microsecond
            // or so: wait for them here rather than going round poll() again.
            FromVenue r;
            while (inflight_ || from_venue_.try_pop(r)) {
                if (inflight_ && !from_venue_.try_pop(r)) {
                    cpu_relax();
                    continue;
                }
                inflight_ -= r.last;
                if (r.has_rep) on_report(r.rep, now());
            }
            if (t >= next_timer) {
                for (size_t i = 0; i < conns_.size(); ++i)
                    if (conns_[i] && !conns_[i]->s.on_timer(t)) drop(i);
                next_timer = t + 100'000'000;
            }
            for (size_t i = 0; i < conns_.size(); ++i) flush(i);
        }
        matcher.join();
    }

    // Received a new order -> its first execution report written to the socket.
    const Histogram& in_to_out() const { return in_to_out_; }
    uint64_t orders() const { return orders_.size(); }

    static Nanos now() {
        return Nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
    }

private:
    struct Conn {
        int fd;
        Session s;
        std::unordered_map<std::string, OrderId> by_cl;  // every ClOrdID this client has used
    };
    struct Ord {
        uint32_t conn;
        std::string cl, pending_cl;  // current ClOrdID; the cancel/replace in flight
        SymbolId sym;
        Side side;
        Qty qty = 0, cum = 0;
        double notional = 0;
        Price pending_px = 0;
        Qty pending_qty = 0;
        Nanos received = 0;  // until its first report goes out
        bool done = false;   // filled, cancelled, rejected or expired
    };
    struct ToVenue {
        VenueRequest rq;
    };
    struct FromVenue {
        ExecReport rep;
        bool has_rep;  // false: the request produced no report (marker only)
        bool last;     // the last report for one request
    };

    static void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

    void accept_all(Nanos) {
        for (;;) {
            int fd = accept(listen_, nullptr, nullptr);
            if (fd < 0) return;
            nonblocking(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // no Nagle: send each message now
            conns_.push_back(std::make_unique<Conn>(Conn{fd, Session({comp_, "", 30'000'000'000, false}), {}}));
        }
    }

    void drop(size_t i) {
        if (!conns_[i]) return;
        close(conns_[i]->fd);
        conns_[i].reset();
    }

    void flush(size_t i) {
        Conn* c = conns_[i].get();
        if (!c) return;
        std::string& out = c->s.output();
        while (!out.empty()) {
            ssize_t n = write(c->fd, out.data(), out.size());
            if (n <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return drop(i);
                return;  // socket full: POLLOUT will bring us back
            }
            out.erase(0, size_t(n));
        }
        if (c->s.state() == Session::State::LoggedOut) drop(i);
    }

    // Matching thread: requests in, execution reports out; market data isn't published here.
    void match(const std::atomic<bool>& stop) {
        Venue venue;
        for (size_t i = 0; i < symbols_.size(); ++i) venue.add_symbol();
        VenueOut out;
        ToVenue r;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!to_venue_.try_pop(r)) {
                cpu_relax();
                continue;
            }
            venue.set_time(now());
            venue.on_request(r.rq, out);
            if (out.reports.empty()) out.reports.push_back({});  // still close the request
            for (size_t i = 0; i < out.reports.size(); ++i) {
                FromVenue f{out.reports[i], i + 1 < out.reports.size() || out.reports[i].id, i + 1 == out.reports.size()};
                while (!from_venue_.try_push(f)) cpu_relax();
            }
            out.clear();
        }
    }

    std::optional<SymbolId> symbol(std::string_view name) const {
        for (size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == name) return SymbolId(i);
        return std::nullopt;
    }

    void submit(const VenueRequest& rq) {
        while (!to_venue_.try_push({rq})) cpu_relax();
        ++inflight_;
    }

    void on_app(uint32_t ci, const fix::Message& m, Nanos t) {
        Conn& c = *conns_[ci];
        std::string_view type = m.type();
        std::string cl(m.get(fix::ClOrdID).value_or(""));
        if (type == "D") {
            std::optional<SymbolId> sym = symbol(m.get(fix::Symbol).value_or(""));
            std::optional<int64_t> qty = m.get_int(fix::OrderQty);
            std::string_view side = m.get(fix::Side).value_or(""), ord = m.get(fix::OrdType).value_or("2");
            std::optional<Price> px = m.get(fix::Price) ? fix::to_price(*m.get(fix::Price)) : std::nullopt;
            const char* why = cl.empty() ? "missing ClOrdID"
                              : c.by_cl.count(cl) ? "duplicate ClOrdID"
                              : !sym ? "unknown symbol"
                              : side != "1" && side != "2" ? "bad Side"
                              : !qty || *qty <= 0 ? "bad OrderQty"
                              : ord != "1" && ord != "2" ? "unsupported OrdType"
                              : ord == "2" && !px ? "bad Price"
                                                  : nullptr;
            if (why) return reject_new(c, cl, why, t);
            Tif tif = Tif::Day;
            std::string_view tv = m.get(fix::TimeInForce).value_or("0");
            if (tv == "1") tif = Tif::Gtc;
            else if (tv == "3") tif = Tif::Ioc;
            else if (tv == "4") tif = Tif::Fok;
            OrderId id = orders_.size() + 1;
            Side s = side == "1" ? Side::Buy : Side::Sell;
            orders_.push_back({ci, cl, "", *sym, s, Qty(*qty), 0, 0, 0, 0, t});
            c.by_cl[cl] = id;
            OrdType ot = ord == "1" ? OrdType::Market : OrdType::Limit;
            // Account 0: no self-trade prevention (opt-in per order on real venues; not wired to FIX here).
            submit(NewOrder{id, 0, *sym, s, ot, ot == OrdType::Market ? Tif::Ioc : tif, px.value_or(0), Qty(*qty)});
            return;
        }
        if (type != "F" && type != "G") return;  // ponytail: other app messages ignored, not rejected
        std::string orig(m.get(fix::OrigClOrdID).value_or(""));
        auto it = c.by_cl.find(orig);
        bool replace = type == "G";
        if (cl.empty() || c.by_cl.count(cl)) return cancel_reject(c, cl, orig, replace, "bad or duplicate ClOrdID", t);
        if (it == c.by_cl.end()) return cancel_reject(c, cl, orig, replace, "unknown order", t);
        Ord& o = orders_[it->second - 1];
        if (!o.pending_cl.empty()) return cancel_reject(c, cl, orig, replace, "request already pending", t);
        c.by_cl[cl] = it->second;
        o.pending_cl = cl;
        if (!replace) return submit(CancelRequest{it->second, o.sym});
        std::optional<int64_t> qty = m.get_int(fix::OrderQty);
        std::optional<Price> px = m.get(fix::Price) ? fix::to_price(*m.get(fix::Price)) : std::nullopt;
        if (!qty || !px || Qty(*qty) <= o.cum) {
            o.pending_cl.clear();
            return cancel_reject(c, cl, orig, true, "bad Price or OrderQty", t);
        }
        o.pending_px = *px;
        o.pending_qty = Qty(*qty);
        submit(ReplaceRequest{it->second, o.sym, *px, Qty(*qty) - o.cum});  // FIX OrderQty is the total
    }

    void reject_new(Conn& c, const std::string& cl, const char* why, Nanos t) {
        c.s.send("8", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, "NONE").field(fix::ClOrdID, cl).field(fix::ExecID, ++exec_id_);
            w.field(fix::ExecType, '8').field(fix::OrdStatus, '8').field(fix::LeavesQty, 0).field(fix::CumQty, 0);
            w.field(fix::AvgPx, 0).field(fix::Text, why);
        });
    }

    void cancel_reject(Conn& c, const std::string& cl, const std::string& orig, bool replace, const char* why,
                       Nanos t, char status = '0') {
        c.s.send("9", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, "NONE").field(fix::ClOrdID, cl).field(fix::OrigClOrdID, orig);
            w.field(fix::OrdStatus, status).field(fix::CxlRejResponseTo, replace ? '2' : '1');
            w.field(fix::CxlRejReason, 1).field(fix::Text, why);
        });
    }

    static char status(const Ord& o, Qty leaves) { return !leaves && o.cum ? '2' : o.cum ? '1' : '0'; }
    // OrdStatus as far as the gateway knows, for a cancel/replace reject.
    static char status(const Ord& o) { return !o.done ? (o.cum ? '1' : '0') : o.cum >= o.qty ? '2' : '4'; }

    void on_report(const ExecReport& r, Nanos t) {
        Ord& o = orders_[r.id - 1];
        Conn* c = conns_[o.conn].get();
        if (r.type == ExecType::Trade) {
            o.cum += r.last_qty;
            o.notional += double(r.last_px) * double(r.last_qty);
        }
        o.done |= (r.type == ExecType::Trade && !r.leaves) || r.type == ExecType::Cancelled ||
                  r.type == ExecType::Rejected || r.type == ExecType::Expired;
        if (!c) return;  // the client is gone; its order still trades
        if (r.type == ExecType::CancelRejected || r.type == ExecType::ReplaceRejected) {
            cancel_reject(*c, o.pending_cl, o.cl, r.type == ExecType::ReplaceRejected,
                          o.done ? "order already done" : "rejected by venue", t, status(o));
            o.pending_cl.clear();
            return;
        }
        std::string orig;
        if (r.type == ExecType::Cancelled && !o.pending_cl.empty()) orig = o.cl, o.cl = o.pending_cl;
        if (r.type == ExecType::Replaced) orig = o.cl, o.cl = o.pending_cl, o.qty = o.pending_qty;
        if (r.type == ExecType::Cancelled || r.type == ExecType::Replaced) o.pending_cl.clear();
        char exec = '0', ord;
        switch (r.type) {
            case ExecType::New: exec = '0'; break;
            case ExecType::Trade: exec = 'F'; break;
            case ExecType::Cancelled: exec = '4'; break;
            case ExecType::Replaced: exec = '5'; break;
            case ExecType::Rejected: exec = '8'; break;
            case ExecType::Expired: exec = 'C'; break;
            case ExecType::Triggered: exec = 'L'; break;
            default: return;
        }
        Qty leaves = r.type == ExecType::Trade || r.type == ExecType::New || r.type == ExecType::Replaced ? r.leaves : 0;
        ord = exec == '4' ? '4' : exec == '8' ? '8' : exec == 'C' ? 'C' : status(o, leaves);
        const std::string& name = symbols_[o.sym];
        c->s.send("8", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, r.id).field(fix::ClOrdID, o.cl);
            if (!orig.empty()) w.field(fix::OrigClOrdID, orig);
            w.field(fix::ExecID, ++exec_id_).field(fix::ExecType, exec).field(fix::OrdStatus, ord);
            w.field(fix::Symbol, name).field(fix::Side, o.side == Side::Buy ? '1' : '2');
            w.field(fix::OrderQty, o.qty).field(fix::LeavesQty, leaves).field(fix::CumQty, o.cum);
            w.price(fix::AvgPx, o.cum ? Price(o.notional / double(o.cum) + 0.5) : 0);
            if (exec == 'F') w.price(fix::LastPx, r.last_px).field(fix::LastQty, r.last_qty);
            if (exec == '8') w.field(fix::Text, "rejected by venue");
        });
        if (o.received) {
            flush(o.conn);
            in_to_out_.record(now() - o.received);
            o.received = 0;
        }
    }

    std::vector<std::string> symbols_;
    std::string comp_;
    int listen_ = -1;
    uint16_t port_ = 0;
    std::vector<std::unique_ptr<Conn>> conns_;  // by connection number; null once closed
    std::vector<Ord> orders_;                   // by OrderId - 1
    uint64_t exec_id_ = 0;
    size_t inflight_ = 0;  // requests sent to the matcher, not yet fully answered
    Spsc<ToVenue, 1 << 16> to_venue_;
    Spsc<FromVenue, 1 << 16> from_venue_;
    Histogram in_to_out_;
};
