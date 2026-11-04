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
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/histogram.hpp"
#include "core/spsc.hpp"
#include "exchange/venue.hpp"
#include "fix/session.hpp"
#include "journal/journal.hpp"

// FIX order entry for the venue. One thread runs a poll() loop over the
// listening socket and every client connection: it frames and sequences FIX
// (Session), turns NewOrderSingle (D), OrderCancelRequest (F) and
// OrderCancelReplaceRequest (G) into venue requests, and turns the venue's
// execution reports back into ExecutionReport (8) and OrderCancelReject (9).
// OrderMassStatusRequest (AF) answers with the status of every order the
// client has. The matching engine runs on its own thread behind two SPSC
// queues, so a slow client never stalls matching.
//
// Sessions belong to a client's CompID, not to a connection: a client that
// reconnects gets its session back, sequence numbers and all.
//
// With a journal, the gateway is recoverable. Every inbound message a session
// processes and every outbound message it writes are journaled, and the
// journal is flushed before any byte reaches a socket (write-ahead: no client
// ever sees an acknowledgement the journal doesn't have). Restarting from the
// journal restores every session (Session::restore) and replays the inbound
// application messages through the same code, straight into the matching
// engine, which rebuilds the books and the order table exactly.
//
// Clients authenticate on Logon (Username/Password against GatewayConfig::users);
// orders are checked against hard per-order limits before they reach the book.
// Sessions are never shared: one connection per CompID at a time.
//
// Both threads busy-poll: lowest latency, one core each.
// ponytail: no cancel-on-disconnect: a client's resting orders outlive its
// connection; add it when a client asks for it.
struct GatewayConfig {
    uint16_t port = 0;  // 0: any free port
    std::string comp_id = "EXCH";
    // CompID -> password. A Logon must carry Username (553) = its SenderCompID
    // and the matching Password (554), or the connection is closed before any
    // session attaches. No entry, no access.
    std::unordered_map<std::string, std::string> users;
    std::string journal;   // empty = none; if it has records, the gateway recovers from them first
    bool fsync = false;    // force the journal to disk before replying, not just to the OS
    size_t max_held = 10'000;  // per session: messages held beyond a sequence gap
};

class Gateway {
public:
    // Per-order limits at the venue's door, whatever the client's own risk says.
    static constexpr int64_t kMaxQty = 10'000'000;
    static constexpr Price kMaxPrice = 100'000'000;  // ticks: $1,000,000.00

    Gateway(std::vector<std::string> symbols, GatewayConfig cfg)
        : symbols_(std::move(symbols)), comp_(cfg.comp_id), fsync_(cfg.fsync), users_(std::move(cfg.users)),
          max_held_(cfg.max_held) {
        uint16_t port = cfg.port;
        for (size_t i = 0; i < symbols_.size(); ++i) venue_.add_symbol();
        if (!cfg.journal.empty()) {
            recover(cfg.journal);
            journal_ = std::make_unique<Journal>(cfg.journal);
        }
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
            if (c) close(c->fd);
        close(listen_);
    }
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    uint16_t port() const { return port_; }

    // Serves until stop is set.
    void run(const std::atomic<bool>& stop) {
        std::atomic<bool> matcher_stop{false};
        std::thread matcher([&] { match(matcher_stop); });
        std::vector<pollfd> fds;
        char buf[1 << 16];
        Nanos next_timer = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fds.assign(1, {listen_, POLLIN, 0});
            for (auto& c : conns_) {
                bool out = c && c->client >= 0 && !clients_[size_t(c->client)]->s.output().empty();
                fds.push_back({c ? c->fd : -1, short(out ? POLLIN | POLLOUT : POLLIN), 0});
            }
            if (poll(fds.data(), nfds_t(fds.size()), 0) < 0) continue;
            Nanos t = now();
            if (fds[0].revents & POLLIN) accept_all();
            for (size_t i = 1; i < fds.size(); ++i) {
                if (!conns_[i - 1] || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t n = read(conns_[i - 1]->fd, buf, sizeof buf);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) drop(i - 1);
                    continue;
                }
                on_bytes(i - 1, buf, size_t(n), t);
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
                    if (Client* c = client_of(i); c && !c->s.on_timer(t)) drop(i);
                next_timer = t + 100'000'000;
            }
            persist();  // write-ahead: the journal has it before any client sees it
            for (size_t i = 0; i < conns_.size(); ++i) flush(i);
        }
        matcher_stop = true;
        matcher.join();
        if (journal_) journal_->flush();
    }

    // Received a new order -> its first execution report written to the socket.
    const Histogram& in_to_out() const { return in_to_out_; }
    uint64_t orders() const { return orders_.size(); }
    uint64_t auth_failures() const { return auth_failures_; }
    // FNV-1a over every execution report the matching engine produced, in
    // order: a recovered gateway must arrive at the same value.
    uint64_t checksum() const { return checksum_; }
    // FNV-1a over both sides of every book, level by level.
    uint64_t book_digest() const {
        uint64_t h = 1469598103934665603ull;
        for (SymbolId s = 0; s < symbols_.size(); ++s)
            for (Side side : {Side::Buy, Side::Sell})
                for (const LevelChange& l : venue_.book(s).top(side, 1 << 20)) {
                    h = fnv(h, &l.price, sizeof l.price);  // field by field: struct padding is garbage
                    h = fnv(h, &l.qty, sizeof l.qty);
                }
        return h;
    }

    static Nanos now() {
        return Nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
    }

private:
    enum Rec : uint16_t { kClient = 1, kIn = 2, kOut = 3 };  // journal record types

    struct Client {
        std::string comp;
        Session s;
        std::unordered_map<std::string, OrderId> by_cl;  // every ClOrdID this client has used
        int conn = -1;                                   // connection it is on, if any
    };
    struct Conn {
        int fd;
        int client = -1;  // unknown until its first message names a CompID
        std::string pre;  // bytes before that
    };
    struct Ord {
        uint32_t client;
        std::string cl, pending_cl;  // current ClOrdID; the cancel/replace in flight
        SymbolId sym;
        Side side;
        Qty qty = 0, cum = 0;
        double notional = 0;
        Price pending_px = 0;
        Qty pending_qty = 0;
        Nanos received = 0;  // until its first report goes out
        bool done = false;   // filled, cancelled, rejected or expired
        bool rejected = false;
    };
    struct ToVenue {
        VenueRequest rq;
    };
    struct FromVenue {
        ExecReport rep;
        bool has_rep;  // false: the request produced no report (marker only)
        bool last;     // the last report for one request
    };

    static uint64_t fnv(uint64_t h, const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }
    static void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

    void persist() {
        if (journal_) fsync_ ? journal_->sync() : journal_->flush();
    }

    Client* client_of(size_t conn) {
        return conns_[conn] && conns_[conn]->client >= 0 ? clients_[size_t(conns_[conn]->client)].get() : nullptr;
    }

    uint32_t add_client(const std::string& comp) {
        uint32_t i = uint32_t(clients_.size());
        clients_.push_back(std::make_unique<Client>(
            Client{comp, Session({comp_, comp, 30'000'000'000, false, "", max_held_}), {}, -1}));
        clients_.back()->s.on_record = [this, i](bool out, std::string_view raw) {
            if (!journal_) return;
            std::string p(reinterpret_cast<const char*>(&i), sizeof i);
            p += raw;
            redact_password(p);  // a journal is not a credential store
            journal_->append(out ? kOut : kIn, now(), std::string_view(p));
        };
        return i;
    }

    // Journal replay: sessions from their records; books and orders by running
    // the inbound application messages again, synchronously, with no output.
    void recover(const std::string& path) {
        recovering_ = true;
        fix::Message m;
        Journal::read(path, [&](uint16_t type, Nanos t, std::string_view p) {
            if (type == kClient) {
                add_client(std::string(p));
                return;
            }
            uint32_t ci;
            if (p.size() < sizeof ci) return;
            std::memcpy(&ci, p.data(), sizeof ci);
            if (ci >= clients_.size()) return;  // not ours: CRCs catch accidents, not tampering
            std::string_view raw = p.substr(sizeof ci);
            clients_[ci]->s.restore(type == kOut, raw);
            if (!m.parse(raw)) return;
            if (type == kIn) on_app(ci, m, t);
            else exec_id_ = std::max<uint64_t>(exec_id_, uint64_t(m.get_int(fix::ExecID).value_or(0)));  // stay unique
        });
        recovering_ = false;
    }

    void accept_all() {
        for (;;) {
            int fd = accept(listen_, nullptr, nullptr);
            if (fd < 0) return;
            nonblocking(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // no Nagle: send each message now
            conns_.push_back(std::make_unique<Conn>(Conn{fd, -1, {}}));
        }
    }

    // A connection's bytes. The first message names the client (SenderCompID):
    // attach its session, new or recovered, then hand everything to it.
    void on_bytes(size_t ci, const char* p, size_t n, Nanos t) {
        Conn& c = *conns_[ci];
        if (c.client < 0) {
            c.pre.append(p, n);
            if (c.pre.size() > kMaxPreLogon) return drop(ci);  // a Logon is a few hundred bytes
            fix::Frame f = fix::frame(c.pre);
            if (f.status == fix::Frame::More) return;
            fix::Message m;
            if (f.status != fix::Frame::Ok || !m.parse(std::string_view(c.pre).substr(0, f.len)) || m.type() != "A")
                return drop(ci);
            std::string comp(m.get(fix::SenderCompID).value_or(""));
            if (!authenticated(m, comp)) {
                ++auth_failures_;
                return drop(ci);  // no session, no journal record, no reply: nothing to learn
            }
            int found = -1;
            for (size_t i = 0; i < clients_.size(); ++i)
                if (clients_[i]->comp == comp) found = int(i);
            if (found >= 0 && clients_[size_t(found)]->conn >= 0) return drop(ci);  // already connected
            if (found < 0) {
                found = int(add_client(comp));
                if (journal_) journal_->append(kClient, t, std::string_view(comp));
            }
            Client& cl = *clients_[size_t(found)];
            cl.s.reconnect();
            cl.conn = int(ci);
            c.client = found;
            std::string bytes;
            bytes.swap(c.pre);
            return on_bytes(ci, bytes.data(), bytes.size(), t);
        }
        uint32_t who = uint32_t(c.client);
        clients_[who]->s.on_bytes(p, n, t, [&](const fix::Message& m) { on_app(who, m, t); });
    }

    void drop(size_t i) {
        if (!conns_[i]) return;
        if (Client* c = client_of(i)) c->conn = -1;
        close(conns_[i]->fd);
        conns_[i].reset();
    }

    void flush(size_t i) {
        Client* c = client_of(i);
        if (!c) return;
        std::string& out = c->s.output();
        while (!out.empty()) {
            ssize_t n = write(conns_[i]->fd, out.data(), out.size());
            if (n <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return drop(i);
                return;  // socket full: POLLOUT will bring us back
            }
            out.erase(0, size_t(n));
        }
        if (c->s.state() == Session::State::LoggedOut) drop(i);
    }

    // Matching thread: requests in, execution reports out; market data isn't
    // published here. Drains what is queued before stopping.
    void match(const std::atomic<bool>& stop) {
        VenueOut out;
        ToVenue r;
        for (;;) {
            if (!to_venue_.try_pop(r)) {
                if (stop.load(std::memory_order_relaxed)) return;
                cpu_relax();
                continue;
            }
            execute(r.rq, out);
            if (out.reports.empty()) out.reports.push_back({});  // still close the request
            for (size_t i = 0; i < out.reports.size(); ++i) {
                FromVenue f{out.reports[i], i + 1 < out.reports.size() || out.reports[i].id, i + 1 == out.reports.size()};
                while (!from_venue_.try_push(f)) {
                    if (stop.load(std::memory_order_relaxed)) return;
                    cpu_relax();
                }
            }
            out.clear();
        }
    }

    // The matching engine, from either thread: the matcher when live, the
    // constructor while recovering (the matcher isn't running then).
    void execute(const VenueRequest& rq, VenueOut& out) {
        venue_.on_request(rq, out);
        for (const ExecReport& e : out.reports) {  // field by field: struct padding is garbage
            uint64_t f[] = {e.id, e.sym, uint64_t(e.type), uint64_t(e.reason), uint64_t(e.last_px), e.last_qty, e.leaves};
            checksum_ = fnv(checksum_, f, sizeof f);
        }
    }

    void submit(const VenueRequest& rq) {
        if (recovering_) {  // straight into the book, and straight back
            execute(rq, recovery_out_);
            std::vector<ExecReport> reps;
            reps.swap(recovery_out_.reports);
            recovery_out_.clear();
            for (const ExecReport& e : reps) on_report(e, 0);
            return;
        }
        // The matcher may itself be waiting for room in from_venue_, which only
        // this thread empties: keep emptying it while waiting, or both threads
        // wait on each other forever.
        while (!to_venue_.try_push({rq})) {
            FromVenue r;
            if (!from_venue_.try_pop(r)) {
                cpu_relax();
                continue;
            }
            inflight_ -= r.last;
            if (r.has_rep) on_report(r.rep, now());
        }
        ++inflight_;
    }

    static bool qty_ok(std::optional<int64_t> q) { return q && *q > 0 && *q <= kMaxQty; }
    static bool price_ok(std::optional<Price> p) { return p && *p > 0 && *p <= kMaxPrice; }

    // Username must be the SenderCompID; the password is compared in constant
    // time, so response timing doesn't reveal how much of a guess was right.
    bool authenticated(const fix::Message& m, const std::string& comp) const {
        auto it = users_.find(comp);
        std::string_view user = m.get(fix::Username).value_or(""), pw = m.get(fix::Password).value_or("");
        if (it == users_.end() || user != comp) return false;
        const std::string& want = it->second;
        unsigned diff = unsigned(want.size() ^ pw.size());
        for (size_t i = 0; i < want.size(); ++i) diff |= unsigned(uint8_t(want[i]) ^ uint8_t(i < pw.size() ? pw[i] : 0));
        return diff == 0;
    }

    static void redact_password(std::string& raw) {
        size_t at = raw.find("\x01" "554=");
        if (at == std::string::npos) return;
        size_t from = at + 5, to = raw.find(fix::SOH, from);
        raw.replace(from, to - from, "***");
    }

    std::optional<SymbolId> symbol(std::string_view name) const {
        for (size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == name) return SymbolId(i);
        return std::nullopt;
    }

    void on_app(uint32_t ci, const fix::Message& m, Nanos t) {
        Client& c = *clients_[ci];
        std::string_view type = m.type();
        std::string cl(m.get(fix::ClOrdID).value_or(""));
        if (type == "AF") return mass_status(c, ci, std::string(m.get(fix::MassStatusReqID).value_or("")), t);
        if (type == "D") {
            std::optional<SymbolId> sym = symbol(m.get(fix::Symbol).value_or(""));
            std::optional<int64_t> qty = m.get_int(fix::OrderQty);
            std::string_view side = m.get(fix::Side).value_or(""), ord = m.get(fix::OrdType).value_or("2");
            std::optional<Price> px = m.get(fix::Price) ? fix::to_price(*m.get(fix::Price)) : std::nullopt;
            const char* why = cl.empty() ? "missing ClOrdID"
                              : c.by_cl.count(cl) ? "duplicate ClOrdID"
                              : !sym ? "unknown symbol"
                              : side != "1" && side != "2" ? "bad Side"
                              : !qty_ok(qty) ? "bad OrderQty"
                              : ord != "1" && ord != "2" ? "unsupported OrdType"
                              : ord == "2" && !price_ok(px) ? "bad Price"
                                                            : nullptr;
            if (why) return reject_new(c, cl, why, t);
            Tif tif = Tif::Day;
            std::string_view tv = m.get(fix::TimeInForce).value_or("0");
            if (tv == "1") tif = Tif::Gtc;
            else if (tv == "3") tif = Tif::Ioc;
            else if (tv == "4") tif = Tif::Fok;
            OrderId id = orders_.size() + 1;
            Side s = side == "1" ? Side::Buy : Side::Sell;
            orders_.push_back({ci, cl, "", *sym, s, Qty(*qty), 0, 0, 0, 0, recovering_ ? 0 : t});
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
        if (!qty_ok(qty) || !price_ok(px) || Qty(*qty) <= o.cum) {  // checked before the cast to unsigned
            o.pending_cl.clear();
            return cancel_reject(c, cl, orig, true, "bad Price or OrderQty", t);
        }
        o.pending_px = *px;
        o.pending_qty = Qty(*qty);
        submit(ReplaceRequest{it->second, o.sym, *px, Qty(*qty) - o.cum});  // FIX OrderQty is the total
    }

    // Sends nothing while recovering: what went out then is already in the
    // journal and in the session's resend store.
    template <class F>
    void send(Client& c, std::string_view type, Nanos t, F&& fill) {
        if (!recovering_) c.s.send(type, t, fill);
    }

    void reject_new(Client& c, const std::string& cl, const char* why, Nanos t) {
        send(c, "8", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, "NONE").field(fix::ClOrdID, cl).field(fix::ExecID, ++exec_id_);
            w.field(fix::ExecType, '8').field(fix::OrdStatus, '8').field(fix::LeavesQty, 0).field(fix::CumQty, 0);
            w.field(fix::AvgPx, 0).field(fix::Text, why);
        });
    }

    void cancel_reject(Client& c, const std::string& cl, const std::string& orig, bool replace, const char* why,
                       Nanos t, char status = '0') {
        send(c, "9", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, "NONE").field(fix::ClOrdID, cl).field(fix::OrigClOrdID, orig);
            w.field(fix::OrdStatus, status).field(fix::CxlRejResponseTo, replace ? '2' : '1');
            w.field(fix::CxlRejReason, 1).field(fix::Text, why);
        });
    }

    static char status(const Ord& o, Qty leaves) { return !leaves && o.cum ? '2' : o.cum ? '1' : '0'; }
    // OrdStatus as far as the gateway knows, for a cancel/replace reject or a status request.
    static char status(const Ord& o) {
        if (!o.done) return o.cum ? '1' : '0';
        return o.cum >= o.qty ? '2' : o.rejected ? '8' : '4';
    }

    // One ExecutionReport (ExecType I, order status) per order the client has
    // ever placed; the last one carries LastRptRequested=Y. No orders: one
    // report with OrderID NONE, so the client still knows it is complete.
    void mass_status(Client& c, uint32_t ci, const std::string& req, Nanos t) {
        std::vector<OrderId> mine;
        for (OrderId id = 1; id <= orders_.size(); ++id)
            if (orders_[id - 1].client == ci) mine.push_back(id);
        if (mine.empty()) {
            return send(c, "8", t, [&](fix::Writer& w) {
                w.field(fix::OrderID, "NONE").field(fix::ExecID, ++exec_id_).field(fix::ExecType, 'I');
                w.field(fix::OrdStatus, '8').field(fix::MassStatusReqID, req).field(fix::TotNumReports, 0);
                w.field(fix::LastRptRequested, 'Y');
            });
        }
        for (size_t k = 0; k < mine.size(); ++k) {
            const Ord& o = orders_[mine[k] - 1];
            send(c, "8", t, [&](fix::Writer& w) {
                w.field(fix::OrderID, mine[k]).field(fix::ClOrdID, o.cl).field(fix::ExecID, ++exec_id_);
                w.field(fix::ExecType, 'I').field(fix::OrdStatus, status(o)).field(fix::Symbol, symbols_[o.sym]);
                w.field(fix::Side, o.side == Side::Buy ? '1' : '2').field(fix::OrderQty, o.qty);
                w.field(fix::LeavesQty, o.done ? 0 : o.qty - o.cum).field(fix::CumQty, o.cum);
                w.price(fix::AvgPx, o.cum ? Price(o.notional / double(o.cum) + 0.5) : 0);
                w.field(fix::MassStatusReqID, req).field(fix::TotNumReports, mine.size());
                if (k + 1 == mine.size()) w.field(fix::LastRptRequested, 'Y');
            });
        }
    }

    void on_report(const ExecReport& r, Nanos t) {
        Ord& o = orders_[r.id - 1];
        if (r.type == ExecType::Trade) {
            o.cum += r.last_qty;
            o.notional += double(r.last_px) * double(r.last_qty);
        }
        o.done |= (r.type == ExecType::Trade && !r.leaves) || r.type == ExecType::Cancelled ||
                  r.type == ExecType::Rejected || r.type == ExecType::Expired;
        o.rejected |= r.type == ExecType::Rejected;
        Client& c = *clients_[o.client];
        if (r.type == ExecType::CancelRejected || r.type == ExecType::ReplaceRejected) {
            cancel_reject(c, o.pending_cl, o.cl, r.type == ExecType::ReplaceRejected,
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
        // A client that is away still gets the report: it waits in the session
        // (and the journal) for a resend after it logs back on.
        send(c, "8", t, [&](fix::Writer& w) {
            w.field(fix::OrderID, r.id).field(fix::ClOrdID, o.cl);
            if (!orig.empty()) w.field(fix::OrigClOrdID, orig);
            w.field(fix::ExecID, ++exec_id_).field(fix::ExecType, exec).field(fix::OrdStatus, ord);
            w.field(fix::Symbol, name).field(fix::Side, o.side == Side::Buy ? '1' : '2');
            w.field(fix::OrderQty, o.qty).field(fix::LeavesQty, leaves).field(fix::CumQty, o.cum);
            w.price(fix::AvgPx, o.cum ? Price(o.notional / double(o.cum) + 0.5) : 0);
            if (exec == 'F') w.price(fix::LastPx, r.last_px).field(fix::LastQty, r.last_qty);
            if (exec == '8') w.field(fix::Text, "rejected by venue");
        });
        if (o.received && c.conn >= 0) {
            persist();  // write-ahead, even on this fast path
            flush(size_t(c.conn));
            in_to_out_.record(now() - o.received);
        }
        o.received = 0;
    }

    static constexpr size_t kMaxPreLogon = 4096;

    std::vector<std::string> symbols_;
    std::string comp_;
    bool fsync_;
    std::unordered_map<std::string, std::string> users_;
    size_t max_held_;
    uint64_t auth_failures_ = 0;
    int listen_ = -1;
    uint16_t port_ = 0;
    Venue venue_;                                   // the matcher's once run() starts
    std::vector<std::unique_ptr<Client>> clients_;  // by first logon
    std::vector<std::unique_ptr<Conn>> conns_;      // by connection number; null once closed
    std::vector<Ord> orders_;                       // by OrderId - 1
    uint64_t exec_id_ = 0;
    uint64_t checksum_ = 1469598103934665603ull;  // written by the matcher; read after run()
    size_t inflight_ = 0;  // requests sent to the matcher, not yet fully answered
    std::unique_ptr<Journal> journal_;
    bool recovering_ = false;
    VenueOut recovery_out_;
    Spsc<ToVenue, 1 << 16> to_venue_;
    Spsc<FromVenue, 1 << 16> from_venue_;
    Histogram in_to_out_;
};
