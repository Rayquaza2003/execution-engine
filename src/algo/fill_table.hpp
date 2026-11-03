#pragma once
#include <cstdlib>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include "core/types.hpp"

// P(fill) lookup from the S6 queue study (research/fill_prob.csv): the chance
// a passive order at the touch fills within a horizon, given the shares queued
// ahead of it and its latency. One symbol per table.
class FillTable {
public:
    // Loads the rows for `sym`; false if the stream has none.
    bool load(std::istream& in, const std::string& sym) {
        std::string line;
        std::getline(in, line);  // header: sym,queue_lo,queue_hi,latency_ns,horizon_ns,n,p_fill
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[7];
            for (std::string& x : f) std::getline(ss, x, ',');
            if (f[0] != sym) continue;
            rows_.push_back({std::strtoull(f[1].c_str(), nullptr, 10), std::strtoull(f[3].c_str(), nullptr, 10),
                             std::strtoull(f[4].c_str(), nullptr, 10), std::strtod(f[6].c_str(), nullptr)});
        }
        return !rows_.empty();
    }

    // The bucket holding `ahead`, at the latency and horizon in the table
    // nearest to the ones asked for. 0 for an empty table.
    double p_fill(Qty ahead, Nanos latency, Nanos horizon) const {
        Nanos lat = nearest(&Row::latency, latency), hor = nearest(&Row::horizon, horizon);
        double p = 0;
        for (const Row& r : rows_)
            if (r.latency == lat && r.horizon == hor && ahead >= r.lo) p = r.p;  // rows ascend by queue_lo
        return p;
    }

private:
    struct Row {
        Qty lo;  // bucket's lowest queue ahead; it runs up to the next row's
        Nanos latency, horizon;
        double p;
    };

    Nanos nearest(Nanos Row::*field, Nanos want) const {
        Nanos best = 0, gap = ~Nanos(0);
        for (const Row& r : rows_) {
            Nanos v = r.*field, d = v > want ? v - want : want - v;
            if (d < gap) gap = d, best = v;
        }
        return best;
    }

    std::vector<Row> rows_;
};
