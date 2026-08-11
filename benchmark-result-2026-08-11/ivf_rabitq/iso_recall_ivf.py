#!/usr/bin/env python3
"""Iso-Recall@10 QPS comparison for IVF-RaBitQ (zvec vs faiss).

Interpolates each engine's Recall-QPS curve (log-linear in QPS) at fixed
Recall@10 targets, per search-thread count.
"""
import json
import math

RES = "/home/jiliang.ljl/benchmark-results/2026-08-11/ivf_rabitq"
THREADS = [1, 4, 8, 16]
TARGETS = {
    ("cohere", 1): [0.60, 0.70, 0.75, 0.80],
    ("cohere", 7): [0.80, 0.90, 0.95, 0.98],
    ("gist", 1): [0.40, 0.50, 0.60, 0.65],
    ("gist", 7): [0.80, 0.90, 0.95, 0.98],
}


def curve(path, thread):
    pts = []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            d = json.loads(line)
            if d["type"] == "search" and d["threads"] == thread:
                pts.append((d["recall_at_k"], d["qps"]))
    return sorted(pts)


def qps_at(pts, target):
    for (r1, q1), (r2, q2) in zip(pts, pts[1:]):
        if r1 <= target <= r2:
            if r2 == r1:
                return max(q1, q2)
            w = (target - r1) / (r2 - r1)
            return math.exp(math.log(q1) + w * (math.log(q2) - math.log(q1)))
    return None


def main():
    out = []
    for dataset in ("cohere", "gist"):
        for bits in (1, 7):
            out.append(f"\n=== {dataset} {bits}-bit iso-Recall@10 QPS ===")
            out.append(
                f"{'recall':>7}{'threads':>9}{'zvec_qps':>10}"
                f"{'faiss_qps':>11}{'zvec/faiss':>12}"
            )
            for target in TARGETS[(dataset, bits)]:
                for thread in THREADS:
                    z = qps_at(curve(f"{RES}/{dataset}-zvec-{bits}bit.jsonl", thread), target)
                    f = qps_at(curve(f"{RES}/{dataset}-faiss-{bits}bit.jsonl", thread), target)
                    zs = f"{z:>10.0f}" if z else f"{'-':>10}"
                    fs = f"{f:>11.0f}" if f else f"{'-':>11}"
                    rs = f"{z / f:>11.2f}x" if (z and f) else f"{'-':>12}"
                    out.append(f"{target:>7.2f}{thread:>9}{zs}{fs}{rs}")
    text = "\n".join(out)
    print(text)
    with open(f"{RES}/iso_recall.txt", "w") as fh:
        fh.write(text + "\n")


if __name__ == "__main__":
    main()
