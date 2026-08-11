#!/usr/bin/env python3
"""Summarize IVF-RaBitQ zvec vs faiss JSONL results (per dataset, per bits)."""
import json
import math
import sys

RES = "/home/jiliang.ljl/benchmark-results/2026-08-11/ivf_rabitq"
NPROBES = [1, 2, 4, 8, 16, 32, 64, 128, 256]
THREADS = [1, 4, 8, 16]


def load(path):
    build = None
    rows = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            d = json.loads(line)
            if d["type"] == "build":
                build = d
            else:
                rows[(d["threads"], d["nprobe"])] = (d["qps"], d["recall_at_k"])
    return build, rows


def qps_at_recall(rows, thread, target):
    """log-linear interpolation of QPS at a given recall on one thread curve."""
    pts = sorted(
        (r, q) for (t, _), (q, r) in rows.items() if t == thread and r is not None
    )
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
            bz, z = load(f"{RES}/{dataset}-zvec-{bits}bit.jsonl")
            bf, f = load(f"{RES}/{dataset}-faiss-{bits}bit.jsonl")
            out.append(f"\n########## {dataset} / {bits}-bit ##########")
            out.append(
                f"{'engine':<7}{'train_s':>9}{'encode_s':>10}"
                f"{'build_s':>9}{'dump_s':>8}{'total_s':>9}"
            )
            for tag, b in (("zvec", bz), ("faiss", bf)):
                out.append(
                    f"{tag:<7}{b['train_seconds']:>9.1f}{b['encode_seconds']:>10.1f}"
                    f"{b['build_seconds']:>9.1f}{b['dump_seconds']:>8.2f}"
                    f"{b['total_seconds']:>9.1f}"
                )
            for thread in THREADS:
                out.append(f"--- {dataset} {bits}-bit, {thread} search threads ---")
                out.append(
                    f"{'nprobe':>7}{'zvec_qps':>11}{'faiss_qps':>11}{'z/f':>7}"
                    f"{'zvec_rec':>10}{'faiss_rec':>11}{'faiss@zrec':>12}{'iso_z/f':>9}"
                )
                for nprobe in NPROBES:
                    key = (thread, nprobe)
                    if key not in z or key not in f:
                        continue
                    qz, rz = z[key]
                    qf, rf = f[key]
                    iso = qps_at_recall(f, thread, rz) if rz is not None else None
                    iso_s = f"{iso:>12.0f}" if iso else f"{'-':>12}"
                    ratio_s = f"{qz / iso:>9.2f}" if iso else f"{'-':>9}"
                    out.append(
                        f"{nprobe:>7}{qz:>11.0f}{qf:>11.0f}{qz / qf:>7.2f}"
                        f"{rz:>10.4f}{rf:>11.4f}{iso_s}{ratio_s}"
                    )
            for tag, rows in (("zvec", z), ("faiss", f)):
                base = rows[(1, 256)][0]
                scale = " ".join(
                    f"{t}t={rows[(t, 256)][0] / base:.1f}x" for t in THREADS
                )
                out.append(f"scaling@nprobe256 {tag:<6}: {scale}")
    text = "\n".join(out)
    print(text)
    with open(f"{RES}/summary.txt", "w") as fh:
        fh.write(text + "\n")


if __name__ == "__main__":
    sys.exit(main())
