# Copyright 2025-present the zvec project
# SPDX-License-Identifier: Apache-2.0
"""Measure one reader and one writer on an open, unsealed FTS Collection."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from itertools import islice
from pathlib import Path
from time import perf_counter_ns

import zvec


def latencies(samples):
    ordered = sorted(samples)
    return {
        "count": len(ordered),
        "ops_per_second": len(ordered) * 1e9 / sum(ordered),
        **{
            key: ordered[min(len(ordered) - 1, int(q * len(ordered)))] / 1000
            for key, q in [("p50_us", 0.5), ("p95_us", 0.95), ("p99_us", 0.99)]
        },
    }


def checked(status):
    if not status.ok():
        raise RuntimeError(str(status))


def save_hits(output, query_id, hits):
    ranked = ",".join(hit.id + ":" + struct.pack("<f", hit.score).hex() for hit in hits)
    output.write(query_id + "\t" + ranked + "\n")


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--collection",
        type=Path,
        required=True,
        help="Empty collection from fts_bench -mode=writing -cmd=create",
    )
    parser.add_argument("--writer", choices=["rocksdb", "runs"], required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--initial", type=int, default=40000)
    parser.add_argument("--windows", type=int, default=100)
    parser.add_argument("--writes", type=int, default=100)
    parser.add_argument("--reads", type=int, default=10)
    args = parser.parse_args()
    if min(args.initial, args.windows, args.writes, args.reads) < 1:
        parser.error("initial, windows, writes and reads must be positive")
    return args


def load_inputs(args):
    size = args.initial + args.windows * args.writes
    docs = []
    with args.corpus.open() as source:
        for line in islice(source, size):
            row = json.loads(line)
            text = row["title"] + " " + row["text"] if row.get("title") else row["text"]
            docs.append(zvec.Doc(id=row["_id"], fields={"text": text}))
    count = args.windows * args.reads
    with args.queries.open() as source:
        queries = [json.loads(line) for line in islice(source, count)]
    if len(docs) != size or len(queries) != count:
        raise ValueError("input too small for the requested workload")
    return docs, queries


def check_visibility(db, search):
    key = "zveclivevisibilityprobe"
    old_text, new_text = "zvecliveold", "zveclivenew"
    checked(db.insert(zvec.Doc(id=key, fields={"text": old_text})))
    if [hit.id for hit in search(old_text)] != [key]:
        raise RuntimeError("fresh insert is missing")
    checked(db.upsert(zvec.Doc(id=key, fields={"text": new_text})))
    if search(old_text) or [hit.id for hit in search(new_text)] != [key]:
        raise RuntimeError("replacement is not visible atomically after write")
    checked(db.delete(key))
    if search(new_text):
        raise RuntimeError("deleted row is still visible")


def mixed_queries(args, db, docs, queries, search):
    def read_window(batch, start):
        intervals = []
        start.wait()
        for row in batch:
            begin = perf_counter_ns()
            search(row["text"])
            intervals.append((begin, perf_counter_ns()))
        return intervals

    read_ns, write_ns, windows = [], [], []
    overlap = 0
    frontier_file = args.output / "frontiers.tsv"
    with (
        ThreadPoolExecutor(max_workers=1) as reader,
        frontier_file.open("w") as frontiers,
    ):
        for i in range(args.windows):
            start = threading.Barrier(2)
            reads = queries[i * args.reads : (i + 1) * args.reads]
            first = args.initial + i * args.writes
            future = reader.submit(read_window, reads, start)
            start.wait()
            write_start = perf_counter_ns()
            for doc in docs[first : first + args.writes]:
                begin = perf_counter_ns()
                checked(db.insert(doc))
                write_ns.append(perf_counter_ns() - begin)
            write_end = perf_counter_ns()
            intervals = future.result()
            read_ns.extend(b - a for a, b in intervals)
            overlap += sum(a < write_end and b > write_start for a, b in intervals)
            windows.append(
                max(write_end, intervals[-1][1]) - min(write_start, intervals[0][0])
            )
            # Both backends have acknowledged the same inserts at this boundary.
            save_hits(frontiers, reads[0]["_id"], search(reads[0]["text"]))
    return {
        "queries_overlapping_writer": overlap,
        "mixed_window_qps": len(queries) * 1e9 / sum(windows),
        "mixed": latencies(read_ns),
        "write": latencies(write_ns),
    }


def main():
    args = arguments()
    zvec.init(memory_limit_mb=256, query_threads=1, optimize_threads=1)
    docs, queries = load_inputs(args)
    args.output.mkdir(parents=True, exist_ok=False)
    options = zvec.CollectionOption(read_only=False, enable_mmap=True)
    db = zvec.open(str(args.collection), option=options)
    if db.stats.doc_count != 0:
        raise ValueError("the collection must be empty before the first write")

    def search(text):
        query = zvec.Query(
            field_name="text",
            fts=zvec.Fts(match_string=text),
            param=zvec.FtsQueryParam(default_operator="OR"),
        )
        return db.query(query, topk=100, output_fields=[])

    check_visibility(db, search)
    for offset in range(0, args.initial, 100):
        for status in db.insert(docs[offset : min(offset + 100, args.initial)]):
            checked(status)
    mixed = mixed_queries(args, db, docs, queries, search)

    # All writers now have the same row frontier. Save ordered IDs and FP32
    # bits outside the timer so independently executed backends can be compared.
    idle_ns = []
    with (args.output / "idle.tsv").open("w") as results:
        for row in queries:
            begin = perf_counter_ns()
            hits = search(row["text"])
            idle_ns.append(perf_counter_ns() - begin)
            save_hits(results, row["_id"], hits)
    if docs[-1].id not in db.fetch(docs[-1].id):
        raise RuntimeError("final write is not visible")
    db.close()
    metrics = {
        "argv": sys.argv,
        "python": sys.executable,
        "writer": args.writer,
        "visibility_probe": True,
        "initial": args.initial,
        "final": len(docs),
        "windows": args.windows,
        "writes_per_window": args.writes,
        "reads_per_window": args.reads,
        "idle": latencies(idle_ns),
        **mixed,
    }
    (args.output / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")


if __name__ == "__main__":
    main()
