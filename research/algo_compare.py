# %% [markdown]
# # Execution algos on real ITCH: transaction cost analysis
#
# Reads the CSV from `./build/algo_compare FILE research/fill_prob.csv algos.csv SYMBOL...`:
# one row per (algo, parent order). Every algo works exactly the same parent
# orders, so algos are compared *paired*, parent by parent, against TWAP. That
# removes the market's own drift, which is shared, and is far bigger than the
# differences between algos.
#
# `python3 research/algo_compare.py algos.csv`

# %%
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from paired_stats import paired

HERE = Path(__file__).resolve().parent
d = pd.read_csv(sys.argv[1] if len(sys.argv) > 1 else "algos.csv")
ORDER = ["TWAP", "VWAP", "POV", "Iceberg", "IS", "Adaptive"]
n = d.groupby("algo").size()
print(f"{n.iloc[0]} parent orders per algo, {d.sym.nunique()} symbols, "
      f"median size {d[d.algo == 'TWAP'].qty.median():.0f} shares")

# %% [markdown]
# ## Cost breakdown
#
# Basis points of the arrival mid, positive = cost. Shortfall = spread +
# timing + opportunity (every parent completed here, so opportunity is 0).

# %%
cols = ["shortfall_bps", "spread_bps", "timing_bps", "vs_vwap_bps", "reversion_bps",
        "passive_share", "children", "duration_s"]
summary = d.groupby("algo")[cols].mean().reindex(ORDER)
print(summary.round(2).to_string())

# %% [markdown]
# ## Paired against TWAP
#
# Mean of (algo - TWAP) over the same parents. Parents are 1-minute windows
# started every 10 s, so neighbouring starts overlap and all symbols at one
# start share the market, so the iid standard error can be too small. Three
# estimates (see paired_stats.py); the chart's 95% interval uses the largest.

# %%
key = ["sym", "start_ns"]
twap = d[d.algo == "TWAP"].set_index(key)
rows = []
for algo in ORDER[1:]:
    x = d[d.algo == algo].set_index(key)
    r = {"algo": algo}
    for c in ["shortfall_bps", "spread_bps", "timing_bps", "vs_vwap_bps"]:
        diff = (x[c] - twap[c]).dropna()
        st = paired(diff, pd.Series(diff.index.get_level_values("start_ns")), block=6)
        r[c] = st["mean"]
        if c == "shortfall_bps":
            r.update({k: st[k] for k in ["se", "se_iid", "se_clustered", "se_block", "ci_lo", "ci_hi", "n", "starts"]})
    rows.append(r)
paired_tab = pd.DataFrame(rows).set_index("algo")
print(paired_tab.round(2).to_string())

# %%
INK, MUTED, GRID, SURFACE, BLUE = "#1f1f1d", "#6b6a66", "#e6e5e1", "#fcfcfb", "#2a78d6"
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED,
    "text.color": INK, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 2,
    "lines.markersize": 8, "font.size": 10,
})
fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4))
y = np.arange(len(paired_tab))[::-1]
err = [paired_tab.shortfall_bps - paired_tab.ci_lo, paired_tab.ci_hi - paired_tab.shortfall_bps]
a1.errorbar(paired_tab.shortfall_bps, y, xerr=err, fmt="o", color=BLUE, ecolor=BLUE, elinewidth=2, capsize=0)
a1.axvline(0, color=MUTED, linewidth=0.8)
a1.set_yticks(y, paired_tab.index)
a1.grid(axis="y", visible=False)
a1.set_xlabel("shortfall minus TWAP's, bps (95% interval, most conservative SE)")
a1.set_title("Cost vs TWAP on the same parents", loc="left", color=INK)

a2.scatter(summary.passive_share, summary.spread_bps, color=BLUE, s=64, zorder=3)
nudge = {"IS": 7, "TWAP": -7}  # these two sit on top of each other
for algo, r in summary.iterrows():
    a2.annotate(algo, (r.passive_share, r.spread_bps), xytext=(7, nudge.get(algo, 0)), textcoords="offset points",
                va="center", color=INK, fontsize=9)
a2.axhline(0, color=MUTED, linewidth=0.8)
a2.set_xlabel("share of fills from resting orders")
a2.set_ylabel("spread cost, bps")
a2.set_title("Spread cost against how much rests", loc="left", color=INK)
fig.tight_layout()
fig.savefig(HERE / "figures/algos.png", dpi=150)
