# %% [markdown]
# # Queue position, latency and fill probability
#
# Reads the CSV from `./build/fill_study FILE fills.csv SYMBOL...` (one row per
# virtual passive order joining the best bid or offer) and produces:
#
# - fill probability by shares queued ahead, for a few latencies
# - fill probability and 1 s markout against latency, for timer joins and for
#   joins of a freshly opened best level
# - `fill_prob.csv`: the lookup table the S7 adaptive algo and the S8 router read
#
# Cells use the `# %%` percent format, so this opens as a notebook in Jupyter
# (via jupytext) or VS Code, and also runs as a plain script:
# `python3 research/fill_study.py fills.csv`.

# %%
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
csv = sys.argv[1] if len(sys.argv) > 1 else "fills.csv"
d = pd.read_csv(csv)
d["ms"] = d.latency_ns / 1e6
print(f"{len(d)} orders, {d.sym.nunique()} symbols, {d.filled.mean():.1%} filled within the horizon")

# Queue sizes differ a lot by symbol (TSLA's touch is ~100 shares, QQQ's
# ~1000), so pooling share buckets across symbols mixes symbols, not queue
# positions. Bucket each symbol by its own quintiles of queue ahead instead,
# with edges from its timer joins at zero latency.
Q = 5
base = d[(d.new_level == 0) & (d.latency_ns == 0)]
EDGES = {s: np.unique(np.quantile(g.queue_ahead, np.linspace(0, 1, Q + 1))[1:-1]) for s, g in base.groupby("sym")}
d["bucket"] = [int(np.searchsorted(EDGES[s], q, side="right")) for s, q in zip(d.sym, d.queue_ahead)]
HORIZONS_NS = [100_000_000, 1_000_000_000, 10_000_000_000]
for h in HORIZONS_NS:
    d[f"fill_{h}"] = d.filled.astype(bool) & (d.time_to_fill_ns <= h)

# Chart style: recessive axes and grid, 2px lines, 8px markers, text in ink colors.
INK, MUTED, GRID, SURFACE = "#1f1f1d", "#6b6a66", "#e6e5e1", "#fcfcfb"
BLUE, ORANGE = "#2a78d6", "#eb6834"         # categorical slots 1-2
CAT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]  # categorical slots 1-5, one per symbol
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED,
    "text.color": INK, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 2,
    "lines.markersize": 8, "font.size": 10, "legend.frameon": False,
})


def label_end(ax, x, y, text):
    ax.annotate(text, (x, y), xytext=(6, 0), textcoords="offset points", va="center", color=INK, fontsize=9)


# %% [markdown]
# ## Fill probability by queue position
#
# Timer joins only (every 100 ms, regardless of what the book is doing), so the
# queue size is not chosen by the event that triggered the decision.

# %%
timer = d[d.new_level == 0]
H = 1_000_000_000
t0 = timer[timer.latency_ns == 0]
fig, ax = plt.subplots(figsize=(7.5, 4))
for color, (sym, g) in zip(CAT, t0.groupby("sym")):
    p = g.groupby("bucket")[f"fill_{H}"].mean()
    ax.plot(p.index + 1, p.values, marker="o", color=color, label=sym)
ax.set_xticks(range(1, Q + 1), ["shortest", "2", "3", "4", "longest"])
ax.set_xlabel("queue ahead on arrival, quintile within symbol")
ax.set_ylabel("P(fill within 1 s)")
ax.set_ylim(0, 1)
ax.set_title("Further back in the queue, fewer fills (0 latency, timer joins)", loc="left", color=INK)
ax.legend(ncol=5, loc="upper center", bbox_to_anchor=(0.5, -0.18))
fig.tight_layout()
fig.savefig(HERE / "figures/fill_by_queue.png", dpi=150)

tab = t0.groupby(["sym", "bucket"]).agg(
    queue_ahead=("queue_ahead", "median"), p_fill_1s=(f"fill_{H}", "mean"), p_fill_10s=("filled", "mean"),
    markout=("markout_ticks", "mean"))
print("\ntimer joins at 0 latency, by queue quintile within symbol")
print(tab.round(3).to_string())

# %% [markdown]
# ## Latency: fill probability and adverse selection
#
# Markout is the mid 1 s after the fill relative to our price, in ticks, signed
# so positive is good for us. Negative means we were filled just before the
# price moved against us: adverse selection.

# %%
def by_latency(frame):
    return frame.groupby("latency_ns").agg(
        n=("filled", "size"),
        queue_ahead=("queue_ahead", "median"),
        p_fill_1s=(f"fill_{H}", "mean"),
        p_fill_10s=("filled", "mean"),
        markout=("markout_ticks", "mean"),
    )

lat = {name: by_latency(d[d.new_level == flag]) for name, flag in [("timer", 0), ("new level", 1)]}
fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4))
for (name, t), color in zip(lat.items(), [BLUE, ORANGE]):
    x = np.maximum(t.index / 1e6, 1e-3)  # 0 ms drawn at 1 us on the log axis
    a1.plot(x, t.p_fill_1s, marker="o", color=color, label=name)
    a2.plot(x, t.markout, marker="o", color=color, label=name)
    label_end(a1, x[-1], t.p_fill_1s.iloc[-1], name)
    label_end(a2, x[-1], t.markout.iloc[-1], name)
for a, title in [(a1, "P(fill within 1 s)"), (a2, "1 s markout, ticks (positive = good)")]:
    a.set_xscale("log")
    a.set_xticks([1e-3, 1e-2, 1e-1, 1, 10, 100], ["0", "0.01", "0.1", "1", "10", "100"])
    a.set_xlabel("latency, ms")
    a.set_title(title, loc="left", color=INK)
a2.axhline(0, color=MUTED, linewidth=0.8)
a1.legend(title="join trigger")
fig.tight_layout()
fig.savefig(HERE / "figures/latency.png", dpi=150)

for name, t in lat.items():
    print(f"\n{name} joins by latency")
    print(t.round(3).to_string())

# %% [markdown]
# ## Export the lookup table
#
# P(fill within horizon) per symbol, queue bucket and latency, from timer
# joins. A bucket covers queue_lo <= shares ahead < queue_hi (queue_hi empty =
# open-ended).

# %%
rows = []
for h in HORIZONS_NS:
    g = timer.groupby(["sym", "bucket", "latency_ns"])[f"fill_{h}"].agg(["size", "mean"])
    for (sym, b, l), r in g.iterrows():
        e = [0, *EDGES[sym]]
        hi = int(EDGES[sym][b]) if b < len(EDGES[sym]) else ""
        rows.append((sym, int(e[b]), hi, l, h, int(r["size"]), round(r["mean"], 4)))
out = pd.DataFrame(rows, columns=["sym", "queue_lo", "queue_hi", "latency_ns", "horizon_ns", "n", "p_fill"])
out.sort_values(["sym", "horizon_ns", "latency_ns", "queue_lo"]).to_csv(HERE / "fill_prob.csv", index=False)
print(f"\nwrote {len(out)} rows to research/fill_prob.csv")
