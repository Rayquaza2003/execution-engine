#pragma once
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fix/fix.hpp"

// One end of a FIX 4.4 session. Pure: bytes come in through on_bytes(), bytes
// to send pile up in output(), time is passed in, and the caller owns the
// socket. So the same code runs over TCP and in tests over a string.
//
//   - Logon (A) handshake; HeartBtInt (108) agreed by the initiator.
//   - MsgSeqNum (34) checked on every message:
//       equal to expected  -> processed
//       higher             -> a gap: the message is held back, and a
//                             ResendRequest (2) asks for exactly the missing
//                             range; held messages are processed in order
//                             once the gap is filled
//       lower              -> a duplicate if PossDupFlag (43) = Y, ignored;
//                             otherwise fatal: Logout (5), "MsgSeqNum too low"
//   - ResendRequest served from a store of everything sent: application
//     messages go again with PossDupFlag=Y and OrigSendingTime (122); runs of
//     admin messages become one SequenceReset-GapFill (4, 123=Y).
//   - Heartbeat (0) after HeartBtInt of silence; TestRequest (1) when the peer
//     has been silent a little longer; the peer is declared dead if it still
//     says nothing for another interval.
//   - Garbled messages (bad checksum or length) are dropped without touching
//     sequence numbers, as the spec says.
//   - Recovery: on_record sees every message sent and every message
//     processed in sequence; replaying those through restore() after a
//     restart gives back the sequence numbers and the resend store, so a
//     reconnecting peer carries on where it left off. Logon with
//     ResetSeqNumFlag (141=Y) starts both sides again from 1 instead.
//
// ponytail: the resend store keeps every message for the whole session in
// memory; persist it (S11 journal) and trim it at the end of the day.
class Session {
public:
    enum class State { AwaitingLogon, Active, LoggedOut };

    struct Config {
        std::string sender, target;  // acceptor: an empty target takes the peer's Logon SenderCompID
        Nanos heartbeat = 30'000'000'000;
        bool initiator = false;
    };

    explicit Session(Config c) : cfg_(std::move(c)) {}

    // Every message sent (as first written; resends aren't) and every inbound
    // message processed in sequence, as raw bytes. For a journal.
    std::function<void(bool outbound, std::string_view raw)> on_record;

    // Initiator: open the session. reset = ResetSeqNumFlag: both sides start
    // again from 1 and forget what they sent.
    void logon(Nanos now, bool reset = false) {
        if (reset) {
            next_out_ = 1;
            store_.clear();
            if (cfg_.initiator) next_in_ = 1;
        }
        int64_t hb = int64_t(cfg_.heartbeat / 1'000'000'000);
        emit("A", now, [&](fix::Writer& w) {
            w.field(fix::EncryptMethod, 0).field(fix::HeartBtInt, hb);
            if (reset) w.field(fix::ResetSeqNumFlag, 'Y');
        });
    }

    // Rebuild from what on_record saw, in the same order, after a restart.
    void restore(bool outbound, std::string_view raw) {
        fix::Message m;
        if (!m.parse(raw)) return;
        uint64_t seq = uint64_t(m.get_int(fix::MsgSeqNum).value_or(0));
        std::string_view type = m.type();
        if (cfg_.target.empty()) cfg_.target = std::string(m.get(outbound ? fix::TargetCompID : fix::SenderCompID).value_or(""));
        if (outbound) {
            if (type == "A" && m.get(fix::ResetSeqNumFlag) == "Y") store_.clear();
            size_t from = raw.find("\x01" "52=");
            from = raw.find(fix::SOH, from + 1) + 1;
            size_t to = raw.rfind("10=");
            store_.push_back({std::string(type), std::string(raw.substr(from, to - from)),
                              std::string(m.get(fix::SendingTime).value_or("")), is_admin(type)});
            next_out_ = seq + 1;
        } else {
            if (type != "4" || m.get(fix::GapFillFlag) == "Y") next_in_ = seq + 1;
            if (type == "4") next_in_ = std::max(next_in_, uint64_t(m.get_int(fix::NewSeqNo).value_or(0)));
        }
    }

    // The connection dropped: keep sequence numbers and the resend store; the
    // next message each way must be a Logon.
    void reconnect() {
        state_ = State::AwaitingLogon;
        in_.clear();
        out_.clear();
        held_.clear();
        resend_asked_ = sent_logout_ = got_logout_ = false;
        test_sent_ = 0;
    }

    // An application message; fill adds the fields after the standard header.
    template <class F>
    void send(std::string_view type, Nanos now, F&& fill) {
        emit(type, now, fill);
    }

    void logout(Nanos now, std::string_view why = "") {
        emit("5", now, [&](fix::Writer& w) {
            if (!why.empty()) w.field(fix::Text, why);
        });
        sent_logout_ = true;
        if (got_logout_) state_ = State::LoggedOut;
    }

    // Bytes from the wire. app(const fix::Message&) is called for every
    // in-sequence application message, while the views are valid.
    template <class F>
    void on_bytes(const char* p, size_t n, Nanos now, F&& app) {
        in_.append(p, n);
        size_t at = 0;
        while (state_ != State::LoggedOut) {
            fix::Frame f = fix::frame(std::string_view(in_).substr(at));
            if (f.status == fix::Frame::More) break;
            if (f.status == fix::Frame::Garbled || !msg_.parse(std::string_view(in_).substr(at, f.len)))
                ++garbled_;
            else
                handle(msg_, now, app), drain(now, app);
            at += f.len;
        }
        in_.erase(0, at);
    }

    // Heartbeats and liveness. False when the peer has gone quiet for good:
    // the caller should drop the connection.
    bool on_timer(Nanos now) {
        if (state_ != State::Active) return true;
        if (now - last_out_ >= cfg_.heartbeat) emit("0", now, [](fix::Writer&) {});
        if (test_sent_) return now - test_sent_ < cfg_.heartbeat;
        if (now - last_in_ >= cfg_.heartbeat + cfg_.heartbeat / 5) {
            std::string id = "T" + std::to_string(now);
            emit("1", now, [&](fix::Writer& w) { w.field(fix::TestReqID, id); });
            test_sent_ = now;
        }
        return true;
    }

    std::string& output() { return out_; }
    const std::string& peer() const { return cfg_.target; }
    State state() const { return state_; }
    uint64_t next_out() const { return next_out_; }
    uint64_t next_in() const { return next_in_; }
    size_t garbled() const { return garbled_; }
    size_t gaps() const { return gaps_; }
    size_t resent() const { return resent_; }

    // "YYYYMMDD-HH:MM:SS.sss" for a time in ns since the Unix epoch.
    static std::string utc(Nanos t) {
        std::time_t s = std::time_t(t / 1'000'000'000);
        std::tm tm;
        gmtime_r(&s, &tm);
        char b[32];
        std::snprintf(b, sizeof b, "%04d%02d%02d-%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(t / 1'000'000 % 1000));
        return b;
    }

private:
    struct Sent {
        std::string type, body;  // body: the fields after the standard header
        std::string sent_at;     // its SendingTime, for OrigSendingTime on a resend
        bool admin;
    };

    static bool is_admin(std::string_view t) {
        return t == "0" || t == "1" || t == "2" || t == "4" || t == "5" || t == "A";
    }

    void header(std::string_view type, uint64_t seq, std::string_view sent_at) {
        w_.begin(type).field(fix::SenderCompID, cfg_.sender).field(fix::TargetCompID, cfg_.target);
        w_.field(fix::MsgSeqNum, seq).field(fix::SendingTime, sent_at);
    }

    template <class F>
    void emit(std::string_view type, Nanos now, F&& fill) {
        std::string sent_at = utc(now);
        header(type, next_out_, sent_at);
        size_t at = w_.size();
        fill(w_);
        store_.push_back({std::string(type), std::string(w_.body().substr(at)), sent_at, is_admin(type)});
        size_t start = out_.size();
        w_.finish(out_);
        if (on_record) on_record(true, std::string_view(out_).substr(start));
        ++next_out_;
        last_out_ = now;
    }

    // Logon received (in sequence, or early: a Logon is never held back).
    void on_logon(const fix::Message& m, Nanos now, bool reset) {
        if (!cfg_.initiator) {
            if (std::optional<int64_t> hb = m.get_int(fix::HeartBtInt); hb && *hb > 0)
                cfg_.heartbeat = Nanos(*hb) * 1'000'000'000;
            logon(now, reset);
        }
        state_ = State::Active;
    }

    template <class F>
    void handle(const fix::Message& m, Nanos now, F& app) {
        std::string_view type = m.type();
        std::optional<int64_t> seq = m.get_int(fix::MsgSeqNum);
        if (cfg_.target.empty() && state_ == State::AwaitingLogon && type == "A")  // acceptor: learn who called
            cfg_.target = std::string(m.get(fix::SenderCompID).value_or(""));
        if (!seq || m.get(fix::SenderCompID) != cfg_.target || m.get(fix::TargetCompID) != cfg_.sender) {
            ++garbled_;  // ponytail: should be a session-level Reject (3); dropped instead
            return;
        }
        last_in_ = now;
        test_sent_ = 0;
        if (state_ == State::AwaitingLogon && type != "A") return logout(now, "first message must be Logon");

        bool reset = type == "A" && m.get(fix::ResetSeqNumFlag) == "Y";
        if (reset) next_in_ = uint64_t(*seq);  // the peer starts again: so do we
        if (type == "4" && m.get(fix::GapFillFlag) != "Y") {  // SequenceReset-Reset: ignores 34
            std::optional<int64_t> to = m.get_int(fix::NewSeqNo);
            if (to && uint64_t(*to) > next_in_) next_in_ = uint64_t(*to);
            if (on_record) on_record(false, m.raw());
            return;
        }
        if (uint64_t(*seq) < next_in_) {
            if (m.get(fix::PossDupFlag) == "Y") return;  // already have it
            return logout(now, "MsgSeqNum too low");
        }
        if (uint64_t(*seq) > next_in_) {
            ++gaps_;
            if (type == "5") return logout(now);
            if (type == "2" || type == "A") {  // act now, hold only its number
                if (type == "2") serve_resend(m, now);
                else on_logon(m, now, false);  // log on first, then ask for what's missing
                held_[uint64_t(*seq)] = "";
            } else {
                held_.emplace(uint64_t(*seq), std::string(m.raw()));
            }
            ask_resend(now);
            return;
        }

        ++next_in_;
        if (on_record) on_record(false, m.raw());
        if (type == "A") {
            on_logon(m, now, reset);
        } else if (type == "1") {
            std::string id(m.get(fix::TestReqID).value_or(""));
            emit("0", now, [&](fix::Writer& w) { w.field(fix::TestReqID, id); });
        } else if (type == "2") {
            serve_resend(m, now);
        } else if (type == "4") {  // GapFill in sequence: jump ahead
            std::optional<int64_t> to = m.get_int(fix::NewSeqNo);
            if (to && uint64_t(*to) > next_in_) next_in_ = uint64_t(*to);
        } else if (type == "5") {
            got_logout_ = true;
            if (!sent_logout_) logout(now);
            state_ = State::LoggedOut;
        } else if (!is_admin(type)) {
            app(m);
        }
    }

    // Ask once for the missing run below the first held message.
    void ask_resend(Nanos now) {
        if (resend_asked_ || held_.empty()) return;
        uint64_t from = next_in_, to = held_.begin()->first - 1;
        emit("2", now, [&](fix::Writer& w) { w.field(fix::BeginSeqNo, from).field(fix::EndSeqNo, to); });
        resend_asked_ = true;
        asked_to_ = to;
    }

    // Process held messages that are now in sequence; ask again if another gap remains.
    template <class F>
    void drain(Nanos now, F& app) {
        if (resend_asked_ && next_in_ > asked_to_) resend_asked_ = false;
        while (!held_.empty() && state_ != State::LoggedOut) {
            auto it = held_.begin();
            if (it->first < next_in_) {  // skipped by a GapFill, or already resent
                held_.erase(it);
                continue;
            }
            if (it->first > next_in_) break;
            std::string raw = std::move(it->second);
            held_.erase(it);
            if (raw.empty()) {  // a ResendRequest already served
                ++next_in_;
                continue;
            }
            held_msg_.parse(raw);
            handle(held_msg_, now, app);
        }
        if (resend_asked_ && next_in_ > asked_to_) resend_asked_ = false;
        ask_resend(now);
    }

    // Resend what the peer asked for: application messages as possible
    // duplicates, runs of admin messages as one GapFill.
    void serve_resend(const fix::Message& m, Nanos now) {
        uint64_t from = uint64_t(m.get_int(fix::BeginSeqNo).value_or(1));
        uint64_t to = uint64_t(m.get_int(fix::EndSeqNo).value_or(0));
        uint64_t last = next_out_ - 1;
        if (!to || to > last) to = last;
        uint64_t gap_from = 0;
        auto fill_gap = [&](uint64_t upto) {  // admin run [gap_from, upto) becomes one GapFill
            if (!gap_from) return;
            std::string at = utc(now);
            header("4", gap_from, at);
            w_.field(fix::PossDupFlag, 'Y').field(fix::OrigSendingTime, at);
            w_.field(fix::GapFillFlag, 'Y').field(fix::NewSeqNo, upto);
            w_.finish(out_);
            gap_from = 0;
        };
        for (uint64_t s = std::max<uint64_t>(from, 1); s <= to; ++s) {
            const Sent& x = store_[s - 1];
            if (x.admin) {
                if (!gap_from) gap_from = s;
                continue;
            }
            fill_gap(s);
            header(x.type, s, utc(now));
            w_.field(fix::PossDupFlag, 'Y').field(fix::OrigSendingTime, x.sent_at).append(x.body);
            w_.finish(out_);
            ++resent_;
        }
        fill_gap(to + 1);
        last_out_ = now;
    }

    Config cfg_;
    State state_ = State::AwaitingLogon;
    uint64_t next_out_ = 1, next_in_ = 1;
    bool resend_asked_ = false;
    uint64_t asked_to_ = 0;               // end of the range asked for
    std::map<uint64_t, std::string> held_;  // arrived beyond a gap, by MsgSeqNum
    bool sent_logout_ = false, got_logout_ = false;
    Nanos last_in_ = 0, last_out_ = 0, test_sent_ = 0;
    size_t garbled_ = 0, gaps_ = 0, resent_ = 0;
    std::string in_, out_;
    std::vector<Sent> store_;  // by MsgSeqNum - 1
    fix::Writer w_;
    fix::Message msg_, held_msg_;
};
