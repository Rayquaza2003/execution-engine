#pragma once
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "core/types.hpp"

// NASDAQ TotalView-ITCH 5.0, the subset that builds an order book: stock
// directory, add, execute, cancel, delete, replace. Every field is big-endian.
// A file (BinaryFILE format) is a sequence of [uint16 length][message].
// Prices are uint32 in 1/10000 dollars; timestamps are 48-bit ns since midnight.
namespace itch {

struct Header {
    char type;
    uint16_t locate;  // per-day symbol index, assigned by the stock directory
    Nanos ts;
};

struct StockDirectory : Header {  // 'R'
    char stock[8];                // space padded
};
struct Add : Header {  // 'A' (and 'F', which adds an MPID we ignore)
    uint64_t ref;
    Side side;
    uint32_t shares;
    uint32_t price;
};
struct Execute : Header {  // 'E' at the order's price; 'C' at `price`
    uint64_t ref;
    uint32_t shares;
    bool printable;  // 'C' only: false = don't count in volume (already printed elsewhere)
    uint32_t price;  // 'C' only; 0 for 'E'
};
struct Cancel : Header {  // 'X' partial cancel
    uint64_t ref;
    uint32_t shares;
};
struct Delete : Header {  // 'D'
    uint64_t ref;
};
struct Replace : Header {  // 'U': new ref, new size and price, priority lost
    uint64_t old_ref;
    uint64_t new_ref;
    uint32_t shares;
    uint32_t price;
};

template <int N>
inline uint64_t be(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < N; ++i) v = v << 8 | p[i];
    return v;
}

// Calls f(msg) for every message in [p, p + n) of a type above and skips the
// rest. Returns the bytes consumed: a message cut off at the end of the
// buffer is left for the next call, so files can be read in chunks.
template <class F>
size_t parse(const uint8_t* p, size_t n, F&& f) {
    size_t at = 0;
    while (at + 2 <= n) {
        size_t len = be<2>(p + at);
        if (at + 2 + len > n) break;
        const uint8_t* m = p + at + 2;
        at += 2 + len;
        if (len < 11) continue;
        Header h{char(m[0]), uint16_t(be<2>(m + 1)), be<6>(m + 5)};
        switch (h.type) {
            case 'R':
                if (len < 19) break;
                {
                    StockDirectory r{h, {}};
                    std::memcpy(r.stock, m + 11, 8);
                    f(r);
                }
                break;
            case 'A':
            case 'F':
                if (len < 36) break;
                f(Add{h, be<8>(m + 11), m[19] == 'B' ? Side::Buy : Side::Sell, uint32_t(be<4>(m + 20)),
                      uint32_t(be<4>(m + 32))});
                break;
            case 'E':
                if (len < 31) break;
                f(Execute{h, be<8>(m + 11), uint32_t(be<4>(m + 19)), true, 0});
                break;
            case 'C':
                if (len < 36) break;
                f(Execute{h, be<8>(m + 11), uint32_t(be<4>(m + 19)), m[31] == 'Y', uint32_t(be<4>(m + 32))});
                break;
            case 'X':
                if (len < 23) break;
                f(Cancel{h, be<8>(m + 11), uint32_t(be<4>(m + 19))});
                break;
            case 'D':
                if (len < 19) break;
                f(Delete{h, be<8>(m + 11)});
                break;
            case 'U':
                if (len < 35) break;
                f(Replace{h, be<8>(m + 11), be<8>(m + 19), uint32_t(be<4>(m + 27)), uint32_t(be<4>(m + 31))});
                break;
            default: break;
        }
    }
    return at;
}

// Encoder for tests, benches and synthetic feeds. Fields we don't parse are zero.
class Writer {
public:
    std::vector<uint8_t> buf;

    void stock_directory(uint16_t locate, Nanos ts, const std::string& stock) {
        uint8_t* m = msg('R', 39, locate, ts);
        std::memset(m + 11, ' ', 8);
        std::memcpy(m + 11, stock.data(), stock.size() < 8 ? stock.size() : 8);
    }
    void add(uint16_t locate, Nanos ts, uint64_t ref, Side side, uint32_t shares, const char* stock,
             uint32_t price) {
        uint8_t* m = msg('A', 36, locate, ts);
        put<8>(m + 11, ref);
        m[19] = side == Side::Buy ? 'B' : 'S';
        put<4>(m + 20, shares);
        std::memset(m + 24, ' ', 8);
        std::memcpy(m + 24, stock, std::strlen(stock) < 8 ? std::strlen(stock) : 8);
        put<4>(m + 32, price);
    }
    void execute(uint16_t locate, Nanos ts, uint64_t ref, uint32_t shares, uint64_t match) {
        uint8_t* m = msg('E', 31, locate, ts);
        put<8>(m + 11, ref);
        put<4>(m + 19, shares);
        put<8>(m + 23, match);
    }
    void execute_at(uint16_t locate, Nanos ts, uint64_t ref, uint32_t shares, uint64_t match,
                    bool printable, uint32_t price) {
        uint8_t* m = msg('C', 36, locate, ts);
        put<8>(m + 11, ref);
        put<4>(m + 19, shares);
        put<8>(m + 23, match);
        m[31] = printable ? 'Y' : 'N';
        put<4>(m + 32, price);
    }
    void cancel(uint16_t locate, Nanos ts, uint64_t ref, uint32_t shares) {
        uint8_t* m = msg('X', 23, locate, ts);
        put<8>(m + 11, ref);
        put<4>(m + 19, shares);
    }
    void del(uint16_t locate, Nanos ts, uint64_t ref) {
        uint8_t* m = msg('D', 19, locate, ts);
        put<8>(m + 11, ref);
    }
    void replace(uint16_t locate, Nanos ts, uint64_t old_ref, uint64_t new_ref, uint32_t shares,
                 uint32_t price) {
        uint8_t* m = msg('U', 35, locate, ts);
        put<8>(m + 11, old_ref);
        put<8>(m + 19, new_ref);
        put<4>(m + 27, shares);
        put<4>(m + 31, price);
    }

private:
    template <int N>
    static void put(uint8_t* p, uint64_t v) {
        for (int i = N - 1; i >= 0; --i, v >>= 8) p[i] = uint8_t(v);
    }
    uint8_t* msg(char type, uint16_t len, uint16_t locate, Nanos ts) {
        size_t at = buf.size();
        buf.resize(at + 2 + len, 0);
        uint8_t* m = buf.data() + at;
        put<2>(m, len);
        m += 2;
        m[0] = uint8_t(type);
        put<2>(m + 1, locate);
        put<6>(m + 5, ts);
        return m;
    }
};

// Synthetic feed for tests and benches: `symbols` stocks named SYM0, SYM1...
// (locates 1..symbols) and n book messages. Buys rest below a fixed $100 mid
// and sells above it, so the book never crosses. Mix: 45% add, 25% delete,
// 10% partial cancel, 10% execute, 10% replace.
inline void synthetic(Writer& w, size_t n, size_t symbols, uint64_t seed) {
    std::mt19937_64 rng(seed);
    struct Live {
        uint64_t ref;
        uint16_t locate;
        Side side;
        uint32_t shares;
    };
    std::vector<Live> live;
    for (size_t s = 0; s < symbols; ++s) w.stock_directory(uint16_t(s + 1), 0, "SYM" + std::to_string(s));
    auto price = [&](Side side) {
        uint32_t off = 100 * uint32_t(1 + rng() % 50);  // 1-50 cents from the mid
        return side == Side::Buy ? 1'000'000 - off : 1'000'000 + off;
    };
    uint64_t next_ref = 1;
    Nanos ts = 34'200'000'000'000;  // 09:30
    for (size_t i = 0; i < n; ++i) {
        ts += rng() % 2000;
        int k = int(rng() % 100);
        if (live.empty() || k < 45) {
            Live o{next_ref++, uint16_t(1 + rng() % symbols), rng() % 2 ? Side::Buy : Side::Sell,
                   uint32_t(100 * (1 + rng() % 5))};
            w.add(o.locate, ts, o.ref, o.side, o.shares, "SYM", price(o.side));
            live.push_back(o);
            continue;
        }
        size_t j = rng() % live.size();
        Live& o = live[j];
        uint32_t part = uint32_t(1 + rng() % o.shares);
        if (k < 70) {
            w.del(o.locate, ts, o.ref);
            part = o.shares;
        } else if (k < 80) {
            w.cancel(o.locate, ts, o.ref, part);
        } else if (k < 90) {
            w.execute(o.locate, ts, o.ref, part, i);
        } else {
            uint64_t ref = next_ref++;
            uint32_t shares = uint32_t(100 * (1 + rng() % 5));
            w.replace(o.locate, ts, o.ref, ref, shares, price(o.side));
            o.ref = ref;
            o.shares = shares;
            continue;
        }
        if (!(o.shares -= part)) {
            o = live.back();
            live.pop_back();
        }
    }
}

}  // namespace itch
