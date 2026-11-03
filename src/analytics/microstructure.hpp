#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "md/market_data.hpp"

// One bar per symbol per interval: the NBBO and depth at the close, plus the
// flow during the bar. Bars are written only for intervals with activity;
// fill gaps forward in the notebook.
struct Bar {
    Nanos t = 0;  // bar end
    SymbolId sym = 0;
    Quote q;                           // NBBO at the close
    Qty depth_bid = 0, depth_ask = 0;  // best kDepthLevels levels at the close
    int64_t ofi = 0;                   // order flow imbalance over the bar
    uint32_t quotes = 0;               // NBBO changes
    uint32_t trades = 0;
    Qty buy_vol = 0, sell_vol = 0;     // by aggressor side
    double notional = 0;               // sum of price * qty, for the VWAP
};

// Order flow imbalance (Cont, Kukanov and Stoikov, 2014): the net quantity
// added at the best bid minus the net added at the best ask between two NBBO
// snapshots. A rising bid counts its whole new size; a falling bid counts the
// whole old size as removed; asks mirror that. Over short horizons it explains
// mid-price moves better than trade volume does.
inline int64_t ofi_step(const Quote& p, const Quote& q) {
    int64_t e = 0;
    if (q.bid >= p.bid) e += int64_t(q.bid_sz);
    if (q.bid <= p.bid) e -= int64_t(p.bid_sz);
    if (q.ask <= p.ask) e -= int64_t(q.ask_sz);
    if (q.ask >= p.ask) e += int64_t(p.ask_sz);
    return e;
}

class Microstructure {
public:
    static constexpr size_t kDepthLevels = 5;

    Microstructure(const MarketData& md, size_t symbols, Nanos interval)
        : md_(md), interval_(interval), st_(symbols) {}

    // Call before applying an event for symbol s at time t: closes the bar if
    // t is past its end, from the book as it stood until now.
    void advance(SymbolId s, Nanos t, std::vector<Bar>& out) {
        State& st = st_[s];
        if (st.open && t >= st.end) close(s, out);
        if (!st.open) {
            st.open = true;
            st.end = (t / interval_ + 1) * interval_;
        }
    }

    // Call after an event's level updates are applied to the MarketData.
    void on_book(SymbolId s) {
        State& st = st_[s];
        Quote q = md_.nbbo(s);
        if (q.bid != st.prev.bid || q.ask != st.prev.ask || q.bid_sz != st.prev.bid_sz ||
            q.ask_sz != st.prev.ask_sz) {
            ++st.bar.quotes;
            if (st.prev.two_sided() && q.two_sided()) st.bar.ofi += ofi_step(st.prev, q);
            st.prev = q;
        }
    }

    void on_trade(const TradePrint& p) {
        Bar& b = st_[p.sym].bar;
        ++b.trades;
        (p.aggressor == Side::Buy ? b.buy_vol : b.sell_vol) += p.qty;
        b.notional += double(p.price) * double(p.qty);
    }

    // End of data: close every open bar.
    void finish(std::vector<Bar>& out) {
        for (SymbolId s = 0; s < st_.size(); ++s)
            if (st_[s].open) close(s, out);
    }

    static const char* csv_header() {
        return "t_ns,sym,bid,ask,bid_sz,ask_sz,spread,mid,imbalance,microprice,"
               "depth5_bid,depth5_ask,ofi,quotes,trades,buy_vol,sell_vol,vwap\n";
    }

    // Prices in ticks. Spread, mid, imbalance and microprice are blank when
    // a side is empty; vwap is blank without trades.
    static std::string csv_row(const Bar& b, const std::string& name) {
        char buf[512];
        int n = std::snprintf(buf, sizeof buf, "%llu,%s,%lld,%lld,%llu,%llu,", (unsigned long long)b.t,
                              name.c_str(), (long long)b.q.bid, (long long)b.q.ask,
                              (unsigned long long)b.q.bid_sz, (unsigned long long)b.q.ask_sz);
        if (b.q.two_sided()) {
            double bs = double(b.q.bid_sz), as = double(b.q.ask_sz);
            n += std::snprintf(buf + n, sizeof buf - size_t(n), "%lld,%.1f,%.4f,%.4f,",
                               (long long)(b.q.ask - b.q.bid), (double(b.q.bid) + double(b.q.ask)) / 2,
                               (bs - as) / (bs + as), (double(b.q.bid) * as + double(b.q.ask) * bs) / (bs + as));
        } else {
            n += std::snprintf(buf + n, sizeof buf - size_t(n), ",,,,");
        }
        n += std::snprintf(buf + n, sizeof buf - size_t(n), "%llu,%llu,%lld,%u,%u,%llu,%llu,",
                           (unsigned long long)b.depth_bid, (unsigned long long)b.depth_ask, (long long)b.ofi,
                           b.quotes, b.trades, (unsigned long long)b.buy_vol, (unsigned long long)b.sell_vol);
        Qty vol = b.buy_vol + b.sell_vol;
        if (vol) n += std::snprintf(buf + n, sizeof buf - size_t(n), "%.4f", b.notional / double(vol));
        std::snprintf(buf + n, sizeof buf - size_t(n), "\n");
        return buf;
    }

private:
    struct State {
        Bar bar;
        Quote prev;
        bool open = false;
        Nanos end = 0;
    };

    void close(SymbolId s, std::vector<Bar>& out) {
        State& st = st_[s];
        Bar& b = st.bar;
        b.t = st.end;
        b.sym = s;
        b.q = md_.nbbo(s);
        b.depth_bid = md_.depth(s, Side::Buy, kDepthLevels);
        b.depth_ask = md_.depth(s, Side::Sell, kDepthLevels);
        out.push_back(b);
        b = Bar{};
        st.open = false;
    }

    const MarketData& md_;
    Nanos interval_;
    std::vector<State> st_;
};
