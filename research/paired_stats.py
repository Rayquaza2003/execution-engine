"""Standard errors for paired algo / router comparisons.

Parents that start at the same moment trade in the same market, and parents
whose windows overlap share even more, so treating every parent as an
independent sample (iid) overstates precision. Three estimates, from least
to most conservative:

- iid: std / sqrt(n), as if every parent were independent.
- clustered: cluster-robust (CR1) by start time, so the symbols that start
  together count as one draw of the market.
- block bootstrap: resample runs of `block` consecutive start times (circular
  moving blocks), which also keeps the dependence between overlapping windows.

The headline `se` is the largest of the three, and the 95% interval is
mean +- 1.96 se. Each estimate can come out low for its own reason (here,
starts alternate buy and sell, so neighbouring differences partly cancel
inside a bootstrap block), and none of them earns extra precision.
"""
import numpy as np
import pandas as pd


def paired(diff: pd.Series, start: pd.Series, block: int, reps: int = 5000, seed: int = 7) -> dict:
    diff = diff.astype(float)
    n = len(diff)
    mean = diff.mean()
    iid = diff.std(ddof=1) / np.sqrt(n)

    by = pd.DataFrame({"d": diff.values, "t": start.values}).groupby("t")["d"]
    sums, counts = by.sum().sort_index(), by.size().sort_index()
    resid = sums - mean * counts
    g = len(sums)
    clustered = np.sqrt((resid**2).sum() * g / (g - 1)) / n

    rng = np.random.default_rng(seed)
    s, c = sums.values, counts.values
    nblocks = int(np.ceil(g / block))
    means = np.empty(reps)
    for r in range(reps):
        starts = rng.integers(0, g, nblocks)
        idx = (starts[:, None] + np.arange(block)[None, :]).ravel() % g
        means[r] = s[idx].sum() / c[idx].sum()
    block_se = means.std(ddof=1)
    se = max(iid, clustered, block_se)
    return {"mean": mean, "se": se, "se_iid": iid, "se_clustered": clustered, "se_block": block_se,
            "ci_lo": mean - 1.96 * se, "ci_hi": mean + 1.96 * se, "n": n, "starts": g}
