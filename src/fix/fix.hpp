#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "core/types.hpp"

// FIX 4.4 tag=value codec. A message is
//
//   8=FIX.4.4|9=<body length>|35=<type>|...body...|10=<checksum>|
//
// where | is the SOH byte (0x01), BodyLength counts the bytes from the field
// after 9= up to and including the SOH before 10=, and CheckSum is the sum of
// every byte before 10= modulo 256, written as three digits.
//
// Prices are decimal on the wire ("150.25") and integer ticks inside; one
// tick is 1/100 of a unit (a cent).
namespace fix {

constexpr char SOH = '\x01';
constexpr std::string_view kBegin = "8=FIX.4.4\x01" "9=";

// Tags this engine reads or writes.
enum Tag : uint32_t {
    AvgPx = 6, BeginSeqNo = 7, ClOrdID = 11, CumQty = 14, EndSeqNo = 16, ExecID = 17, LastPx = 31,
    LastQty = 32, MsgSeqNum = 34, MsgType = 35, NewSeqNo = 36, OrderID = 37, OrderQty = 38, OrdStatus = 39,
    OrdType = 40, OrigClOrdID = 41, PossDupFlag = 43, Price = 44, SenderCompID = 49, SendingTime = 52,
    Side = 54, Symbol = 55, TargetCompID = 56, Text = 58, TimeInForce = 59, CxlRejReason = 102,
    EncryptMethod = 98, HeartBtInt = 108, ResetSeqNumFlag = 141, TestReqID = 112, OrigSendingTime = 122, GapFillFlag = 123,
    ExecType = 150, LeavesQty = 151, CxlRejResponseTo = 434, MassStatusReqID = 584, MassStatusReqType = 585,
    TotNumReports = 911, LastRptRequested = 912,
};

inline uint8_t checksum(std::string_view s) {
    unsigned sum = 0;
    for (char c : s) sum += uint8_t(c);
    return uint8_t(sum);
}

inline std::optional<int64_t> to_int(std::string_view v) {
    int64_t x = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc() || p != v.data() + v.size()) return std::nullopt;
    return x;
}

// "150.25" -> 15025 ticks. At most two decimals; nullopt if not a price.
inline std::optional<::Price> to_price(std::string_view v) {
    bool neg = !v.empty() && v[0] == '-';
    if (neg) v.remove_prefix(1);
    size_t dot = v.find('.');
    std::string_view whole = v.substr(0, dot), frac = dot == v.npos ? "" : v.substr(dot + 1);
    if (whole.empty() || frac.size() > 2) return std::nullopt;
    std::optional<int64_t> w = to_int(whole);
    if (!w || *w < 0) return std::nullopt;
    int64_t f = 0;
    for (size_t i = 0; i < 2; ++i) {
        if (i < frac.size() && (frac[i] < '0' || frac[i] > '9')) return std::nullopt;
        f = f * 10 + (i < frac.size() ? frac[i] - '0' : 0);
    }
    int64_t t = *w * 100 + f;
    return neg ? -t : t;
}

// One received message, as views into the bytes it was parsed from.
class Message {
public:
    struct Field {
        uint32_t tag;
        std::string_view value;
    };

    // `raw` must be one whole message that frame() accepted.
    bool parse(std::string_view raw) {
        raw_ = raw;
        fields_.clear();
        while (!raw.empty()) {
            size_t eq = raw.find('='), end = raw.find(SOH);
            if (eq == raw.npos || end == raw.npos || eq > end) return false;
            std::optional<int64_t> tag = to_int(raw.substr(0, eq));
            if (!tag || *tag <= 0) return false;
            fields_.push_back({uint32_t(*tag), raw.substr(eq + 1, end - eq - 1)});
            raw.remove_prefix(end + 1);
        }
        return fields_.size() >= 4 && fields_[2].tag == MsgType;
    }

    std::string_view type() const { return fields_[2].value; }
    std::string_view raw() const { return raw_; }
    std::optional<std::string_view> get(uint32_t tag) const {
        for (const Field& f : fields_)
            if (f.tag == tag) return f.value;
        return std::nullopt;
    }
    std::optional<int64_t> get_int(uint32_t tag) const {
        std::optional<std::string_view> v = get(tag);
        return v ? to_int(*v) : std::nullopt;
    }

private:
    std::string_view raw_;
    std::vector<Field> fields_;
};

// The first message in a byte stream.
struct Frame {
    enum Status { More, Ok, Garbled } status;
    size_t len;  // Ok: the message's length; Garbled: bytes to skip; More: 0
};

inline Frame frame(std::string_view buf) {
    if (buf.size() < kBegin.size()) {
        return kBegin.substr(0, buf.size()) == buf ? Frame{Frame::More, 0} : Frame{Frame::Garbled, 1};
    }
    if (buf.substr(0, kBegin.size()) != kBegin) {  // resync at the next BeginString
        size_t next = buf.find("8=FIX.4.4", 1);
        return {Frame::Garbled, next == buf.npos ? buf.size() : next};
    }
    size_t end = buf.find(SOH, kBegin.size());
    if (end == buf.npos) return buf.size() - kBegin.size() > 8 ? Frame{Frame::Garbled, 1} : Frame{Frame::More, 0};
    std::optional<int64_t> body = to_int(buf.substr(kBegin.size(), end - kBegin.size()));
    if (!body || *body <= 0 || *body > 1 << 20) return {Frame::Garbled, 1};
    size_t sum_at = end + 1 + size_t(*body), total = sum_at + 7;  // "10=xxx|"
    if (buf.size() < total) return {Frame::More, 0};
    std::string_view trailer = buf.substr(sum_at, 7);
    if (trailer.substr(0, 3) != "10=" || trailer[6] != SOH) return {Frame::Garbled, 1};
    std::optional<int64_t> sum = to_int(trailer.substr(3, 3));
    if (!sum || *sum != checksum(buf.substr(0, sum_at))) return {Frame::Garbled, total};
    return {Frame::Ok, total};
}

// Builds one message at a time; keeps its buffer, so it stops allocating once warm.
class Writer {
public:
    Writer& begin(std::string_view type) {
        body_.clear();
        return field(MsgType, type);
    }
    Writer& field(uint32_t tag, std::string_view v) {
        tag_(tag);
        body_ += v;
        body_ += SOH;
        return *this;
    }
    Writer& field(uint32_t tag, char v) { return field(tag, std::string_view(&v, 1)); }
    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, char> && !std::is_same_v<I, bool>)
    Writer& field(uint32_t tag, I v) {
        tag_(tag);
        num(v);
        body_ += SOH;
        return *this;
    }
    Writer& price(uint32_t tag, ::Price ticks) {
        tag_(tag);
        if (ticks < 0) body_ += '-', ticks = -ticks;
        num(ticks / 100);
        body_ += '.';
        body_ += char('0' + ticks % 100 / 10);
        body_ += char('0' + ticks % 10);
        body_ += SOH;
        return *this;
    }
    // Raw, already-encoded fields (each ending in SOH).
    Writer& append(std::string_view fields) {
        body_ += fields;
        return *this;
    }
    size_t size() const { return body_.size(); }
    std::string_view body() const { return body_; }

    // Wraps the body in BeginString, BodyLength and CheckSum and appends it to out.
    void finish(std::string& out) const {
        size_t start = out.size();
        out += kBegin;
        char len[16];
        auto r = std::to_chars(len, len + sizeof len, body_.size());
        out.append(len, r.ptr);
        out += SOH;
        out += body_;
        uint8_t sum = checksum(std::string_view(out).substr(start));
        out += "10=";
        out += char('0' + sum / 100);
        out += char('0' + sum / 10 % 10);
        out += char('0' + sum % 10);
        out += SOH;
    }

private:
    void tag_(uint32_t tag) {
        num(tag);
        body_ += '=';
    }
    template <class I>
    void num(I v) {
        char b[24];
        auto r = std::to_chars(b, b + sizeof b, v);
        body_.append(b, r.ptr);
    }
    std::string body_;
};

}  // namespace fix
