#pragma once
#include <cstdint>
#include <cstring>
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

}  // namespace itch
