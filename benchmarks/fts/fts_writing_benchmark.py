"""Run the FTS writing comparison, preserving commands, passes and score bits."""

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import statistics
import subprocess


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--du", type=Path, required=True)
    parser.add_argument("--quora", type=Path, required=True)
    parser.add_argument("--quora-hot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-du", type=Path)
    parser.add_argument("--reference-quora", type=Path)
    parser.add_argument("--reference-rocksdb-du", type=Path)
    parser.add_argument("--reference-rocksdb-quora", type=Path)
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument("--plan", action="store_true", help="Write commands for an external executor")
    operation.add_argument("--collect", action="store_true", help="Verify outputs of the planned commands")
    args = parser.parse_args()
    root = args.output.resolve()
    if not args.collect:
        root.mkdir(exist_ok=False)
    commands = []
    results = {}
    binary = args.bench.resolve()
    binary_hash = digest(binary)
    identity = f"{binary_hash}  {binary}\n"
    if args.collect:
        if (root / "binary.sha256").read_text() != identity:
            raise RuntimeError("The executable differs from the planned benchmark")
    else:
        (root / "binary.sha256").write_text(identity)
    common = ["-mode=index", "-threads=1", "-topk=100", "-default_operator=or",
              "-index_cache_mb=8"]

    def execute(label, flags):
        if not args.plan and not args.collect and digest(binary) != binary_hash:
            raise RuntimeError("The benchmark executable changed during measurement")
        command = [str(binary), *common, *map(str, flags)]
        commands.append({"label": label, "args": command})
        if not args.collect:
            (root / "commands.json").write_text(json.dumps(commands, indent=2))
        if args.plan:
            return {}
        print(label, flush=True)
        if not args.collect:
            with (root / (label + ".txt")).open("x") as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)
        lines = (root / (label + ".txt")).read_text().splitlines()
        metrics = {p[1]: p[2] for p in (s.split("\t") for s in lines)
                   if p[0] == "METRIC"}
        passes = [dict(zip(p[::2], p[1::2])) for p in (s.split("\t") for s in lines)
                  if p[0] == "PASS"]
        if "-cmd=build" in flags and "build_total_ms" not in metrics:
            raise RuntimeError(f"Incomplete build output for {label}")
        if "-cmd=search" in flags:
            wanted = int(next(f.split("=")[1] for f in flags if f.startswith("-index_passes=")))
            if len(passes) != wanted:
                raise RuntimeError(f"Incomplete search output for {label}")
        return {"metrics": metrics, "passes": passes}

    for name, data, tokenizer in [("du", args.du, "jieba"), ("quora", args.quora, "standard")]:
        data = data.resolve()
        record = results[name] = {"corpus_sha256": digest(data / "corpus.jsonl"),
                                  "build": {}, "search": {}, "equality": {}}
        indexes = {}
        for round_id in range(1, 4):
            for backend in (("rocksdb", "runs") if round_id % 2 else ("runs", "rocksdb")):
                label = f"{name}-{backend}-build-{round_id}"
                index = root / (label + "-index")
                indexes[backend] = index
                record["build"][label] = execute(label, [
                    "-cmd=build", f"-index={index}", f"-corpus={data / 'corpus.jsonl'}",
                    f"-extra_params={json.dumps({'tokenizer': tokenizer})}",
                    f"-index_writer={backend}", "-index_wal=true", "-index_mmap=true",
                    "-index_format=" + ("custom" if backend == "runs" else "rocksdb")])
                if not args.plan:
                    (root / "results.json").write_text(json.dumps(results, indent=2))

        selections = [("full", data / "queries.jsonl", 3 if name == "du" else 1,
                       3 if name == "du" else 1)]
        if name == "quora":
            selections.append(("hot", args.quora_hot.resolve(), 1, 3))
        for selection, queries, processes, passes in selections:
            expected = {}
            for process in range(processes):
                modes = ["rocksdb", "mmap", "buffer"]
                if process % 2:
                    modes.reverse()
                for mode in modes:
                    backend = "rocksdb" if mode == "rocksdb" else "runs"
                    index = indexes[backend]
                    if mode == "buffer" and not args.collect and not args.plan:
                        # Touch every mapped page in the custom file before
                        # starting a fresh process with an empty BufferPool.
                        for path in index.glob("*.zfts"):
                            with path.open("rb") as stream:
                                with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                                    for pos in range(0, len(mapped), mmap.PAGESIZE):
                                        _ = mapped[pos]
                    label = f"{name}-{selection}-{mode}-{process + 1}"
                    prefix = root / (label + ".tsv")
                    record["search"][label] = execute(label, [
                        "-cmd=search", f"-index={index}", f"-query={queries}",
                        f"-qrels={data / 'qrels'}", f"-index_passes={passes}",
                        f"-index_mmap={'false' if mode == 'buffer' else 'true'}",
                        f"-result_file={prefix}"])
                    if args.plan:
                        continue
                    hashes = [digest(Path(f"{prefix}.pass{i}")) for i in range(passes)]
                    expected.setdefault(backend, hashes[0])
                    if any(value != expected[backend] for value in hashes):
                        raise RuntimeError(f"Results changed between {label} passes or read modes")
                    record["equality"][label] = hashes
                    reference = (args.reference_du if name == "du" else args.reference_quora)
                    if backend == "rocksdb":
                        reference = (args.reference_rocksdb_du if name == "du"
                                     else args.reference_rocksdb_quora)
                    if selection == "full" and reference:
                        if hashes[0] != digest(reference):
                            raise RuntimeError(f"{label} differs from its same-host {backend} reference")
                    (root / "results.json").write_text(json.dumps(results, indent=2))
            if not args.plan:
                record["equality"][selection + "_cross_backend"] = expected["rocksdb"] == expected["runs"]
        if args.plan:
            continue
        record["medians_ms"] = {}
        for backend in ("rocksdb", "runs"):
            builds = [v["metrics"] for k, v in record["build"].items()
                      if f"-{backend}-" in k]
            record["medians_ms"][backend] = {
                key: statistics.median(float(run[key]) for run in builds)
                for key in ("ingest_flush_ms", "seal_sync_ms", "build_total_ms")
            }
        (root / "results.json").write_text(json.dumps(results, indent=2))
    if args.collect and commands != json.loads((root / "commands.json").read_text()):
        raise RuntimeError("Collected command list differs from the original plan")
    print(root, flush=True)


if __name__ == "__main__":
    main()
