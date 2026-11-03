#pragma once
#include <cmath>
#include <vector>

#include "core/types.hpp"

// Transaction cost analysis for one parent order, Perold-style: everything is
// measured against the mid at the decision (arrival). Ticks per share,
// positive = cost to us, for buys and sells alike.
//
//   shortfall   = execution * filled/target + opportunity       (per target share)
//   execution   = avg fill price vs arrival = spread + timing    (per filled share)
//   spread      = fill price vs the mid at that moment: paid to cross, negative when passive
//   timing      = mid at our fills vs arrival: the market drifting, plus our own impact
//   opportunity = shares never filled, valued at the move from arrival to the end
//   vs_vwap     = avg fill price vs the market's VWAP over the same interval
//   reversion   = mid at the end vs some time later; positive means the price
//                 came back once we stopped: temporary impact we paid for
struct TcaFill {
    Price px;
    Qty qty;
    double mid;  // at the fill
};

struct TcaInput {
    Side side;
    Qty target;
    double arrival_mid, end_mid, post_mid;
    double vwap;  // NaN if the market didn't trade
    std::vector<TcaFill> fills;
};

struct Tca {
    Qty filled = 0;
    double avg_px = NAN;
    double shortfall = NAN, execution = NAN, spread = NAN, timing = NAN, opportunity = NAN;
    double vs_vwap = NAN, reversion = NAN;
};

inline Tca tca(const TcaInput& in) {
    double sg = in.side == Side::Buy ? 1 : -1;
    Tca t;
    double paid = 0, mids = 0;
    for (const TcaFill& f : in.fills) {
        t.filled += f.qty;
        paid += double(f.px) * double(f.qty);
        mids += f.mid * double(f.qty);
    }
    double filled = double(t.filled), target = double(in.target);
    double missed = target > filled ? target - filled : 0;
    t.opportunity = sg * (in.end_mid - in.arrival_mid) * missed / target;
    t.reversion = sg * (in.end_mid - in.post_mid);
    if (t.filled) {
        t.avg_px = paid / filled;
        t.spread = sg * (paid - mids) / filled;
        t.timing = sg * (mids / filled - in.arrival_mid);
        t.execution = t.spread + t.timing;
        t.vs_vwap = sg * (t.avg_px - in.vwap);
    }
    t.shortfall = (t.filled ? t.execution * filled / target : 0) + t.opportunity;
    return t;
}
