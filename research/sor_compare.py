# %% [markdown]
# # Smart order routing over three venues
#
# Reads the CSV from `./build/sor_compare FILE research/fill_prob.csv sor.csv SYMBOL...`:
# one row per (mode, parent order). The three venues are random slices of the
# real NASDAQ book (10% / 60% / 30% of the orders), with different fees and
# latency. Every mode works the same parents, so smart and naive are
# compared parent by parent.
#
# `python3 research/sor_compare.py sor.csv`

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
d = pd.read_csv(sys.argv[1] if len(sys.argv) > 1 else "sor.csv")
MODES = ["SmartTake", "NaiveTake", "SmartPost", "NaivePost"]
print(f"{(d['mode'] == 'SmartTake').sum()} parents per mode, median {d.qty.median():.0f} shares")

# %% [markdown]
# ## Summary
#
# All-in cost = price paid vs the arrival mid, plus fees, in bps; positive = cost.

# %%
cols = ["cost_bps", "fee_bps", "rounds", "time_ms", "passive_share", "share_A", "share_B", "share_C"]
summary = d.groupby("mode")[cols].mean().reindex(MODES)
print(summary.round(3).to_string())

# %% [markdown]
# ## Paired: smart minus naive
#
# Parents start every 5 s and are over within ~2 s, so windows don't overlap,
# but the five symbols at one start share the market. Errors are clustered by
# start time; the block bootstrap (runs of 12 starts = one minute) adds any
# drift that carries across neighbouring starts.

# %%
key = ["sym", "start_ns"]
rows = []
for kind in ["Take", "Post"]:
    s = d[d["mode"] == "Smart" + kind].set_index(key)
    n = d[d["mode"] == "Naive" + kind].set_index(key)
    r = {"kind": kind}
    for c in ["cost_bps", "fee_bps", "passive_share", "rounds"]:
        diff = (s[c] - n[c]).dropna()
        st = paired(diff, pd.Series(diff.index.get_level_values("start_ns")), block=12)
        r[c] = st["mean"]
        r[c + "_se"] = st["se"]
        if c == "cost_bps":
            r.update({k: st[k] for k in ["se", "se_iid", "se_clustered", "se_block", "ci_lo", "ci_hi", "n", "starts"]})
    rows.append(r)
paired_tab = pd.DataFrame(rows).set_index("kind")
print(paired_tab.round(3).to_string())

# %%
INK, MUTED, GRID, SURFACE = "#1f1f1d", "#6b6a66", "#e6e5e1", "#fcfcfb"
VENUE = {"A": "#2a78d6", "B": "#eb6834", "C": "#1baf7a"}  # categorical slots 1-3
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED,
    "text.color": INK, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 2,
    "lines.markersize": 8, "font.size": 10, "legend.frameon": False,
})
fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.8))
y = np.arange(len(MODES))[::-1]
left = np.zeros(len(MODES))
for v, color in VENUE.items():
    w = summary[f"share_{v}"].values
    a1.barh(y, w, left=left, color=color, edgecolor=SURFACE, linewidth=2, height=0.6, label=f"venue {v}")
    for yi, l, wi in zip(y, left, w):  # direct labels: the aqua fill is low contrast on its own
        if wi >= 0.08:
            a1.text(l + wi / 2, yi, f"{wi:.0%}", ha="center", va="center", color=INK, fontsize=9)
    left += w
a1.set_yticks(y, MODES)
a1.set_xlim(0, 1)
a1.grid(axis="y", visible=False)
a1.set_xlabel("share of filled shares")
a1.set_title("Where each router traded", loc="left", color=INK)
a1.legend(ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.2))

y2 = np.arange(len(paired_tab))[::-1]
a2.errorbar(paired_tab.cost_bps, y2, xerr=[paired_tab.cost_bps - paired_tab.ci_lo, paired_tab.ci_hi - paired_tab.cost_bps], fmt="o", color=VENUE["A"],
            ecolor=VENUE["A"], elinewidth=2, capsize=0)
a2.axvline(0, color=MUTED, linewidth=0.8)
a2.set_yticks(y2, ["taking", "resting"])
a2.set_ylim(-0.6, 1.6)
a2.grid(axis="y", visible=False)
a2.set_xlabel("smart minus naive all-in cost, bps (95% interval, most conservative SE)")
a2.set_title("Smart vs naive on the same parents", loc="left", color=INK)
fig.tight_layout()
fig.savefig(HERE / "figures/sor.png", dpi=150)
