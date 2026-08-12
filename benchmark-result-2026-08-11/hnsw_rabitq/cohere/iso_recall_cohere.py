#!/usr/bin/env python3
"""Cohere HNSW iso-Recall@10 comparison across the 0.80-0.95 band.

zvec curves join two runs: ef>=32 from the original index (one-bit/), ef<32 from
the rebuilt index (one-bit-lowef/). ef=32 was measured on both and agrees within
0.005 recall, so the join is treated as one curve; the ef=32 point is always taken
from the original run.

ES is the envelope over all rescore_vector oversample settings: at each target the
best-performing oversample is reported.
"""
import collections
import json
import math

BASE = "/home/jiliang.ljl/benchmark-results/2026-08-11/hnsw_rabitq/cohere"
THREADS = [1, 4, 8, 16]
TARGETS = [0.80, 0.85, 0.90, 0.95, 0.98]


def rows(path):
    with open(path) as fh:
        return [json.loads(line) for line in fh]


def zvec_curves():
    """{(label, threads): [(recall, qps), ...]}"""
    out = collections.defaultdict(list)
    for path, keep in ((f"{BASE}/one-bit/search-results.jsonl", lambda ef: ef >= 32),
                       (f"{BASE}/one-bit-lowef/search-results.jsonl", lambda ef: ef < 32)):
        for r in rows(path):
            if r["engine"] != "zvec" or not keep(r["search_value"]):
                continue
            label = "zvec-1bit+refine" if r["details"]["raw_vector_refine"] else "zvec-1bit"
            out[(label, r["threads"])].append((r["recall_at_k"], max(r["qps_repeats"])))
    for path in (f"{BASE}/seven-bit/search-results.jsonl",):
        for r in rows(path):
            if r["engine"] == "zvec":
                out[("zvec-7bit", r["threads"])].append(
                    (r["recall_at_k"], max(r["qps_repeats"]))
                )
    return {k: sorted(v) for k, v in out.items()}


def es_curves():
    out = collections.defaultdict(list)
    for r in rows(f"{BASE}/one-bit/search-results.jsonl"):
        if r["engine"] != "elasticsearch":
            continue
        ov = r["details"].get("rescore_oversample")
        label = "es-norescore" if ov is None else f"es-ov{ov:g}"
        out[(label, r["threads"])].append((r["recall_at_k"], max(r["qps_repeats"])))
    return {k: sorted(v) for k, v in out.items()}


def qps_at(pts, target):
    """log-linear interpolation; pts must be sorted by recall."""
    best = None
    for (r1, q1), (r2, q2) in zip(pts, pts[1:]):
        if r1 <= target <= r2:
            if r2 == r1:
                value = max(q1, q2)
            else:
                w = (target - r1) / (r2 - r1)
                value = math.exp(math.log(q1) + w * (math.log(q2) - math.log(q1)))
            best = value if best is None else max(best, value)
    return best


def best(curves, target, thread):
    winner, value = None, None
    for (label, t), pts in curves.items():
        if t != thread:
            continue
        got = qps_at(pts, target)
        if got is not None and (value is None or got > value):
            winner, value = label, got
    return winner, value


def main():
    zc, ec = zvec_curves(), es_curves()
    out = ["=== Cohere HNSW iso-Recall@10 QPS (ES = best oversample envelope) ===",
           f"{'recall':>7}{'threads':>8}{'zvec_qps':>10}{'zvec cfg':>19}"
           f"{'es_qps':>8}{'es cfg':>12}{'zvec/es':>9}"]
    for target in TARGETS:
        for thread in THREADS:
            zl, zq = best(zc, target, thread)
            el, eq = best(ec, target, thread)
            zs = f"{zq:>10.0f}" if zq else f"{'-':>10}"
            es = f"{eq:>8.0f}" if eq else f"{'-':>8}"
            rs = f"{zq / eq:>8.1f}x" if (zq and eq) else f"{'-':>9}"
            out.append(
                f"{target:>7.2f}{thread:>8}{zs}{zl or '-':>19}{es}{el or '-':>12}{rs}"
            )
    out.append("\n=== joined curves, threads=1 (param:Recall/QPS) ===")
    for label, curves in (("zvec", zc), ("es", ec)):
        for (name, thread), pts in sorted(curves.items()):
            if thread != 1:
                continue
            out.append(f"{name:18}" + "  ".join(f"{r:.3f}/{q:.0f}" for r, q in pts))
    text = "\n".join(out)
    print(text)
    with open(f"{BASE}/iso_recall_cohere.txt", "w") as fh:
        fh.write(text + "\n")


if __name__ == "__main__":
    main()
