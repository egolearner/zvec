#!/usr/bin/env python3
# Copyright 2025-present the zvec project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Compare Zvec HNSW-RaBitQ with Elasticsearch BBQ-HNSW.

The benchmark consumes an ANN-Benchmarks HDF5 dataset directly. Build and
search can run independently so the persisted indexes can be built once and
searched repeatedly under controlled CPU and memory conditions.
"""
# This command-line benchmark intentionally prints progress and JSON records.
# Optional engine dependencies are imported lazily so inspect/build modes stay independent.
# ruff: noqa: PLC0415, T201

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import math
import os
import platform
import statistics
import sys
import threading
import time
from collections.abc import Callable, Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np

VECTOR_FIELD = "vector"
ZVEC_MARKER = ".hnsw_rabitq_vs_es_benchmark"
MANIFEST_NAME = "manifest.json"
RESULTS_NAME = "search-results.jsonl"
MIN_ELASTICSEARCH_VERSION = (8, 18, 0)


@dataclass(frozen=True)
class DatasetInfo:
    path: str
    file_size: int
    mtime_ns: int
    sha256: str
    distance: str
    base_count: int
    query_count: int
    dimension: int
    groundtruth_k: int


@dataclass
class BuildResult:
    engine: str
    total_seconds: float
    ingest_seconds: float
    finalize_seconds: float
    vectors_per_second: float
    index_bytes: int
    vector_count: int
    dimension: int
    details: dict[str, Any]


@dataclass
class SearchResult:
    engine: str
    search_value_name: str
    search_value: int
    threads: int
    query_count: int
    topk: int
    qps: float
    qps_repeats: list[float]
    throughput_query_counts: list[int]
    throughput_seconds: list[float]
    recall_at_1: float
    recall_at_k: float
    details: dict[str, Any]


@dataclass
class EngineClients:
    zvec: Any = None
    elasticsearch: Any = None
    elasticsearch_helpers: Any = None
    elasticsearch_version: str | None = None


def import_h5py():
    try:
        import h5py
    except ImportError as exc:
        raise RuntimeError(
            "h5py is required; install the benchmark requirements first"
        ) from exc
    return h5py


def configure_local_zvec_path() -> None:
    repo_root = Path(__file__).resolve().parents[3]
    for path in (repo_root / "build.release/lib", repo_root / "python"):
        path_text = str(path)
        if path.exists() and path_text not in sys.path:
            sys.path.insert(0, path_text)


def import_zvec():
    try:
        import zvec
    except ImportError as exc:
        configure_local_zvec_path()
        try:
            import zvec
        except ImportError:
            raise RuntimeError(
                "Zvec is required; build _zvec under build.release/lib or "
                "install the Python package"
            ) from exc
    return zvec


def import_elasticsearch():
    try:
        import elasticsearch
        from elasticsearch import helpers
    except ImportError as exc:
        raise RuntimeError(
            "the official elasticsearch Python client is required for the "
            "Elasticsearch engine"
        ) from exc
    return elasticsearch, helpers


def parse_positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"expected a positive integer, got {value}")
    return parsed


def parse_nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError(
            f"expected a non-negative integer, got {value}"
        )
    return parsed


def parse_positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"expected a positive number, got {value}")
    return parsed


def parse_optional_positive_float(value: str) -> float | None:
    if value.lower() in ("none", "off"):
        return None
    parsed = parse_positive_float(value)
    if parsed <= 1.0 or parsed >= 10.0:
        raise argparse.ArgumentTypeError(
            "rescore oversample must be greater than 1.0 and less than 10.0"
        )
    return parsed


def parse_int_list(value: str) -> list[int]:
    try:
        parsed = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "expected a comma-separated list of integers"
        ) from exc
    if not parsed or any(item <= 0 for item in parsed):
        raise argparse.ArgumentTypeError("all list values must be positive")
    return sorted(set(parsed))


def selected_engines(value: str) -> list[str]:
    if value == "both":
        return ["zvec", "elasticsearch"]
    return [value]


def min_nonzero(limit: int, available: int) -> int:
    return available if limit == 0 else min(limit, available)


def iter_slices(total: int, batch_size: int) -> Iterable[tuple[int, int]]:
    for start in range(0, total, batch_size):
        yield start, min(start + batch_size, total)


def dataset_slice(dataset: Any, start: int, stop: int) -> np.ndarray:
    return np.ascontiguousarray(dataset[start:stop], dtype=np.float32)


def file_sha256(path: Path, chunk_size: int = 16 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_dataset(path: Path) -> DatasetInfo:
    if not path.is_file():
        raise RuntimeError(f"dataset does not exist: {path}")
    stat = path.stat()
    sha256 = file_sha256(path)
    h5py = import_h5py()
    with h5py.File(path, "r") as source:
        required = {"train", "test", "neighbors"}
        missing = required.difference(source.keys())
        if missing:
            raise RuntimeError(f"dataset is missing HDF5 keys: {sorted(missing)}")
        train = source["train"]
        test = source["test"]
        neighbors = source["neighbors"]
        if len(train.shape) != 2 or len(test.shape) != 2:
            raise RuntimeError("train and test must both be rank-2 arrays")
        if len(neighbors.shape) != 2:
            raise RuntimeError("neighbors must be a rank-2 array")
        if train.shape[1] != test.shape[1]:
            raise RuntimeError("train and test dimensions differ")
        if neighbors.shape[0] < test.shape[0]:
            raise RuntimeError("neighbors contains fewer rows than test")
        if not np.issubdtype(train.dtype, np.floating):
            raise RuntimeError(
                f"train must contain floating-point vectors: {train.dtype}"
            )
        if not np.issubdtype(test.dtype, np.floating):
            raise RuntimeError(
                f"test must contain floating-point vectors: {test.dtype}"
            )
        if not np.issubdtype(neighbors.dtype, np.integer):
            raise RuntimeError(f"neighbors must contain integer IDs: {neighbors.dtype}")
        distance = source.attrs.get("distance", "")
        if isinstance(distance, bytes):
            distance = distance.decode("utf-8")
        distance = str(distance).lower()
        if distance not in ("euclidean", "l2"):
            raise RuntimeError(
                f"this benchmark currently supports only Euclidean distance, got "
                f"{distance!r}"
            )
        current_stat = path.stat()
        if (
            current_stat.st_size != stat.st_size
            or current_stat.st_mtime_ns != stat.st_mtime_ns
        ):
            raise RuntimeError(f"dataset changed while it was being inspected: {path}")
        return DatasetInfo(
            path=str(path.resolve()),
            file_size=stat.st_size,
            mtime_ns=stat.st_mtime_ns,
            sha256=sha256,
            distance="euclidean",
            base_count=int(train.shape[0]),
            query_count=int(test.shape[0]),
            dimension=int(train.shape[1]),
            groundtruth_k=int(neighbors.shape[1]),
        )


def selected_dataset_info(
    dataset_info: DatasetInfo, max_base: int, max_queries: int
) -> DatasetInfo:
    return DatasetInfo(
        path=dataset_info.path,
        file_size=dataset_info.file_size,
        mtime_ns=dataset_info.mtime_ns,
        sha256=dataset_info.sha256,
        distance=dataset_info.distance,
        base_count=min_nonzero(max_base, dataset_info.base_count),
        query_count=min_nonzero(max_queries, dataset_info.query_count),
        dimension=dataset_info.dimension,
        groundtruth_k=dataset_info.groundtruth_k,
    )


def validate_search_args(info: DatasetInfo, args: argparse.Namespace) -> None:
    if info.base_count < args.topk:
        raise RuntimeError(
            f"selected base count {info.base_count} is smaller than topk={args.topk}"
        )
    if args.topk > info.groundtruth_k:
        raise RuntimeError(
            f"topk={args.topk} exceeds ground truth width {info.groundtruth_k}"
        )
    invalid = [value for value in args.search_values if value < args.topk]
    if invalid:
        raise RuntimeError(
            f"search values must be at least topk={args.topk}: {invalid}"
        )
    if args.es_rescore_oversample is not None:
        required_candidates = math.ceil(args.topk * args.es_rescore_oversample)
        invalid_es = [
            value for value in args.search_values if value < required_candidates
        ]
        if invalid_es and "elasticsearch" in selected_engines(args.engines):
            raise RuntimeError(
                "Elasticsearch num_candidates must cover the rescore window "
                f"k*oversample={required_candidates}: {invalid_es}"
            )


def validate_ground_truth(ground_truth: np.ndarray, base_count: int, topk: int) -> None:
    selected = ground_truth[:, :topk]
    if selected.size == 0:
        raise RuntimeError("ground truth is empty")
    minimum = int(np.min(selected))
    maximum = int(np.max(selected))
    if minimum < 0 or maximum >= base_count:
        raise RuntimeError(
            f"ground truth IDs [{minimum}, {maximum}] do not fit selected "
            f"base count {base_count}; use the complete base set"
        )


def compute_recall(
    labels: np.ndarray, ground_truth: np.ndarray, topk: int
) -> tuple[float, float]:
    if labels.shape != (ground_truth.shape[0], topk):
        raise ValueError(
            f"labels shape {labels.shape} does not match "
            f"({ground_truth.shape[0]}, {topk})"
        )
    recall_at_1 = float(np.mean(labels[:, 0] == ground_truth[:, 0]))
    hits = 0
    for actual, expected in zip(labels, ground_truth[:, :topk], strict=False):
        hits += len(set(map(int, actual)).intersection(map(int, expected)))
    return recall_at_1, hits / (labels.shape[0] * topk)


def exact_ground_truth(
    train: Any,
    queries: np.ndarray,
    base_count: int,
    topk: int,
    block_size: int = 10000,
) -> np.ndarray:
    """Compute exact squared-L2 neighbors for a truncated smoke-test base."""
    best_distances = np.full((queries.shape[0], topk), np.inf, dtype=np.float32)
    best_labels = np.full((queries.shape[0], topk), -1, dtype=np.int64)
    query_norms = np.sum(queries * queries, axis=1, keepdims=True)
    for start, stop in iter_slices(base_count, block_size):
        base = dataset_slice(train, start, stop)
        base_norms = np.sum(base * base, axis=1)
        distances = query_norms + base_norms - 2.0 * queries @ base.T
        labels = np.broadcast_to(
            np.arange(start, stop, dtype=np.int64), distances.shape
        )
        candidate_distances = np.concatenate((best_distances, distances), axis=1)
        candidate_labels = np.concatenate((best_labels, labels), axis=1)
        selected = np.argpartition(candidate_distances, topk - 1, axis=1)[:, :topk]
        best_distances = np.take_along_axis(candidate_distances, selected, axis=1)
        best_labels = np.take_along_axis(candidate_labels, selected, axis=1)
    order = np.argsort(best_distances, axis=1)
    return np.take_along_axis(best_labels, order, axis=1)


def path_size(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    if not path.exists():
        return 0
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def load_manifest(work_dir: Path) -> dict[str, Any]:
    path = work_dir / MANIFEST_NAME
    if not path.exists():
        return {"build": {}}
    return json.loads(path.read_text(encoding="utf-8"))


def dataset_mismatches(
    saved: dict[str, Any],
    info: DatasetInfo,
    allow_relocation: bool,
) -> list[str]:
    ignored = {"path", "mtime_ns"} if allow_relocation else set()
    mismatches = [
        f"{key}: saved={saved.get(key)!r}, requested={value!r}"
        for key, value in asdict(info).items()
        if key not in ignored
        and not (key == "sha256" and saved.get(key) is None)
        and saved.get(key) != value
    ]
    if allow_relocation and saved.get("sha256") is None:
        mismatches.append(
            "sha256: persisted build has no dataset hash; rebuild before relocation"
        )
    return mismatches


def validate_manifest(
    manifest: dict[str, Any], info: DatasetInfo, args: argparse.Namespace
) -> None:
    saved = manifest.get("dataset")
    if saved is None:
        raise RuntimeError("manifest has no dataset metadata; run build first")
    mismatches = dataset_mismatches(
        saved,
        info,
        args.allow_dataset_relocation,
    )
    saved_config = manifest.get("config", {})
    config_keys = ["m", "ef_construction"]
    if "zvec" in selected_engines(args.engines):
        config_keys.extend(
            ("zvec_total_bits", "zvec_num_clusters", "zvec_sample_count")
        )
    for key in config_keys:
        requested_value = getattr(args, key)
        if saved_config.get(key) != requested_value:
            mismatches.append(
                f"{key}: saved={saved_config.get(key)!r}, requested={requested_value!r}"
            )
    if "elasticsearch" in selected_engines(args.engines):
        saved_index = (
            manifest.get("build", {})
            .get("elasticsearch", {})
            .get("details", {})
            .get("index")
        )
        if saved_index != args.es_index:
            mismatches.append(
                f"es_index: saved={saved_index!r}, requested={args.es_index!r}"
            )
    if mismatches:
        raise RuntimeError(
            "persisted build does not match this search: " + "; ".join(mismatches)
        )


def validate_retained_builds(
    manifest: dict[str, Any],
    info: DatasetInfo,
    args: argparse.Namespace,
    rebuilt_engines: Sequence[str],
) -> None:
    retained = set(manifest.get("build", {})).difference(rebuilt_engines)
    if not retained:
        return
    saved_dataset = manifest.get("dataset", {})
    mismatches = dataset_mismatches(
        saved_dataset,
        info,
        args.allow_dataset_relocation,
    )
    saved_config = manifest.get("config", {})
    for key in ("m", "ef_construction"):
        value = getattr(args, key)
        if saved_config.get(key) != value:
            mismatches.append(
                f"{key}: saved={saved_config.get(key)!r}, requested={value!r}"
            )
    if mismatches:
        raise RuntimeError(
            f"new build would invalidate retained engines {sorted(retained)}: "
            + "; ".join(mismatches)
            + "; use a separate --work-dir or rebuild both engines"
        )


def initialize_zvec(args: argparse.Namespace) -> Any:
    zvec = import_zvec()
    zvec.init(
        query_threads=max(args.search_threads),
        optimize_threads=args.build_threads,
    )
    return zvec


def remove_existing_zvec(path: Path, overwrite: bool, zvec: Any) -> None:
    if not path.exists():
        return
    if not overwrite:
        raise RuntimeError(
            f"Zvec collection already exists: {path}; use --overwrite or search mode"
        )
    marker = path / ZVEC_MARKER
    if not marker.is_file():
        raise RuntimeError(f"refusing to destroy unrecognized Zvec collection: {path}")
    collection = zvec.open(str(path))
    collection.destroy()


def ensure_statuses_ok(statuses: Sequence[Any], operation: str) -> None:
    for offset, status in enumerate(statuses):
        if not status.ok():
            raise RuntimeError(f"{operation} failed at batch offset {offset}: {status}")


def build_zvec(
    source: Any,
    work_dir: Path,
    info: DatasetInfo,
    args: argparse.Namespace,
    zvec: Any,
) -> BuildResult:
    path = work_dir / "zvec"
    remove_existing_zvec(path, args.overwrite, zvec)
    started = time.perf_counter()
    schema = zvec.CollectionSchema(
        name="gist_hnsw_rabitq",
        vectors=[
            zvec.VectorSchema(
                VECTOR_FIELD,
                zvec.DataType.VECTOR_FP32,
                dimension=info.dimension,
                index_param=zvec.HnswRabitqIndexParam(
                    metric_type=zvec.MetricType.L2,
                    total_bits=args.zvec_total_bits,
                    num_clusters=args.zvec_num_clusters,
                    sample_count=args.zvec_sample_count,
                    m=args.m,
                    ef_construction=args.ef_construction,
                ),
            )
        ],
    )
    collection = zvec.create_and_open(
        str(path),
        schema,
        zvec.CollectionOption(read_only=False, enable_mmap=True),
    )
    (path / ZVEC_MARKER).write_text(
        "created by tools/benchmark/hnsw_rabitq_vs_es/benchmark.py\n",
        encoding="utf-8",
    )

    ingest_started = time.perf_counter()
    for start, stop in iter_slices(info.base_count, args.zvec_batch_size):
        vectors = dataset_slice(source["train"], start, stop)
        docs = [
            zvec.Doc(id=str(doc_id), vectors={VECTOR_FIELD: vector})
            for doc_id, vector in zip(range(start, stop), vectors, strict=False)
        ]
        ensure_statuses_ok(collection.insert(docs), f"Zvec insert [{start}:{stop})")
        print(f"\rZvec insert: {stop}/{info.base_count}", end="", flush=True)
    print()
    ingest_seconds = time.perf_counter() - ingest_started

    finalize_started = time.perf_counter()
    collection.optimize(zvec.OptimizeOption(concurrency=args.build_threads))
    collection.flush()
    finalize_seconds = time.perf_counter() - finalize_started
    total_seconds = time.perf_counter() - started
    del collection
    gc.collect()
    return BuildResult(
        engine="zvec",
        total_seconds=total_seconds,
        ingest_seconds=ingest_seconds,
        finalize_seconds=finalize_seconds,
        vectors_per_second=info.base_count / total_seconds,
        index_bytes=path_size(path),
        vector_count=info.base_count,
        dimension=info.dimension,
        details={
            "path": str(path),
            "index_type": "HNSW_RABITQ",
            "metric": "L2",
            "m": args.m,
            "ef_construction": args.ef_construction,
            "total_bits": args.zvec_total_bits,
            "num_clusters": args.zvec_num_clusters,
            "sample_count": args.zvec_sample_count,
            "build_threads": args.build_threads,
            "storage_scope": "entire Zvec collection directory",
        },
    )


def parse_version(value: str) -> tuple[int, int, int]:
    parts = value.split(".")
    try:
        parsed = tuple(int(part.split("-", 1)[0]) for part in parts[:3])
    except ValueError as exc:
        raise RuntimeError(f"cannot parse Elasticsearch version {value!r}") from exc
    if len(parsed) != 3:
        raise RuntimeError(f"cannot parse Elasticsearch version {value!r}")
    return parsed


def create_es_client(args: argparse.Namespace) -> tuple[Any, Any, str]:
    elasticsearch, helpers = import_elasticsearch()
    kwargs: dict[str, Any] = {
        "request_timeout": args.es_request_timeout,
        "verify_certs": not args.es_insecure,
        "connections_per_node": max(args.search_threads),
    }
    if args.es_api_key:
        kwargs["api_key"] = args.es_api_key
    elif args.es_username:
        kwargs["basic_auth"] = (args.es_username, args.es_password or "")
    client = elasticsearch.Elasticsearch(args.es_url, **kwargs)
    info = client.info()
    version = str(info["version"]["number"])
    if parse_version(version) < MIN_ELASTICSEARCH_VERSION:
        raise RuntimeError(
            f"Elasticsearch {version} is too old; version 8.18.0 or newer is required"
        )
    return client, helpers, version


def es_index_definition(info: DatasetInfo, args: argparse.Namespace) -> dict[str, Any]:
    return {
        "settings": {
            "number_of_shards": 1,
            "number_of_replicas": 0,
            "refresh_interval": "-1",
        },
        "mappings": {
            "_source": {"enabled": False},
            "dynamic": "strict",
            "properties": {
                VECTOR_FIELD: {
                    "type": "dense_vector",
                    "element_type": "float",
                    "dims": info.dimension,
                    "index": True,
                    "similarity": "l2_norm",
                    "index_options": {
                        "type": "bbq_hnsw",
                        "m": args.m,
                        "ef_construction": args.ef_construction,
                    },
                }
            },
        },
    }


def es_actions(
    train: Any,
    index_name: str,
    base_count: int,
    batch_rows: int,
) -> Iterable[dict[str, Any]]:
    for start, stop in iter_slices(base_count, batch_rows):
        vectors = dataset_slice(train, start, stop)
        for doc_id, vector in zip(range(start, stop), vectors, strict=False):
            yield {
                "_op_type": "index",
                "_index": index_name,
                "_id": str(doc_id),
                "_source": {VECTOR_FIELD: vector.tolist()},
            }
        print(f"\rElasticsearch submit: {stop}/{base_count}", end="", flush=True)


def es_primary_stats(client: Any, index_name: str) -> tuple[int, int, int]:
    response = client.indices.stats(
        index=index_name, metric=["docs", "store", "segments"]
    )
    primaries = response["_all"]["primaries"]
    return (
        int(primaries["store"]["size_in_bytes"]),
        int(primaries["docs"]["count"]),
        int(primaries["segments"]["count"]),
    )


def wait_for_stable_es_primary_stats(
    client: Any,
    index_name: str,
    timeout: float,
    interval: float = 1.0,
    stable_samples: int = 30,
) -> tuple[tuple[int, int, int], int, float]:
    """Wait for old segment files to disappear before measuring index storage."""
    started = time.perf_counter()
    deadline = started + timeout
    previous: tuple[int, int, int] | None = None
    consecutive = 0
    sample_count = 0
    while True:
        current = es_primary_stats(client, index_name)
        sample_count += 1
        if current == previous:
            consecutive += 1
        else:
            previous = current
            consecutive = 1
        elapsed = time.perf_counter() - started
        if consecutive >= stable_samples:
            return current, sample_count, elapsed
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            raise RuntimeError(
                "Elasticsearch primary store stats did not stabilize within "
                f"{timeout:.1f}s; last stats={current}"
            )
        time.sleep(min(interval, remaining))


def validate_es_index(
    client: Any,
    index_name: str,
    info: DatasetInfo,
    args: argparse.Namespace,
) -> None:
    response = client.indices.get_mapping(index=index_name)
    if index_name not in response:
        raise RuntimeError(
            f"Elasticsearch mapping response has no exact index {index_name!r}"
        )
    field = response[index_name]["mappings"]["properties"].get(VECTOR_FIELD, {})
    index_options = field.get("index_options", {})
    expected = {
        "type": (field.get("type"), "dense_vector"),
        "dims": (field.get("dims"), info.dimension),
        "similarity": (field.get("similarity"), "l2_norm"),
        "index_options.type": (index_options.get("type"), "bbq_hnsw"),
        "index_options.m": (index_options.get("m"), args.m),
        "index_options.ef_construction": (
            index_options.get("ef_construction"),
            args.ef_construction,
        ),
    }
    mismatches = [
        f"{name}: actual={actual!r}, expected={wanted!r}"
        for name, (actual, wanted) in expected.items()
        if actual != wanted
    ]
    if mismatches:
        raise RuntimeError(
            "Elasticsearch index mapping does not match the benchmark: "
            + "; ".join(mismatches)
        )


def build_elasticsearch(
    source: Any,
    info: DatasetInfo,
    args: argparse.Namespace,
    client: Any,
    helpers: Any,
    version: str,
) -> BuildResult:
    exists = bool(client.indices.exists(index=args.es_index))
    if exists and not args.overwrite:
        raise RuntimeError(
            f"Elasticsearch index already exists: {args.es_index}; "
            "use --overwrite or search mode"
        )
    if exists:
        client.indices.delete(index=args.es_index)

    started = time.perf_counter()
    client.indices.create(
        index=args.es_index,
        settings=es_index_definition(info, args)["settings"],
        mappings=es_index_definition(info, args)["mappings"],
    )
    ingest_started = time.perf_counter()
    success_count = 0
    for ok, item in helpers.parallel_bulk(
        client.options(request_timeout=args.es_request_timeout),
        es_actions(
            source["train"],
            args.es_index,
            info.base_count,
            args.es_source_batch_rows,
        ),
        thread_count=args.build_threads,
        queue_size=args.es_bulk_queue_size,
        chunk_size=args.es_bulk_docs,
        max_chunk_bytes=args.es_bulk_bytes,
        raise_on_error=True,
        raise_on_exception=True,
    ):
        if not ok:
            raise RuntimeError(f"Elasticsearch bulk item failed: {item}")
        success_count += 1
    print()
    if success_count != info.base_count:
        raise RuntimeError(
            f"Elasticsearch indexed {success_count} documents, "
            f"expected {info.base_count}"
        )
    ingest_seconds = time.perf_counter() - ingest_started

    finalize_started = time.perf_counter()
    client.indices.refresh(index=args.es_index)
    if not args.es_skip_force_merge:
        client.options(request_timeout=args.es_force_merge_timeout).indices.forcemerge(
            index=args.es_index,
            max_num_segments=1,
        )
        client.indices.refresh(index=args.es_index)
    finalize_seconds = time.perf_counter() - finalize_started
    total_seconds = time.perf_counter() - started
    (
        (index_bytes, doc_count, segment_count),
        store_stats_samples,
        store_stabilization_seconds,
    ) = wait_for_stable_es_primary_stats(
        client,
        args.es_index,
        timeout=args.es_store_stability_timeout,
    )
    if doc_count != info.base_count:
        raise RuntimeError(
            f"Elasticsearch reports {doc_count} documents, expected {info.base_count}"
        )
    return BuildResult(
        engine="elasticsearch",
        total_seconds=total_seconds,
        ingest_seconds=ingest_seconds,
        finalize_seconds=finalize_seconds,
        vectors_per_second=info.base_count / total_seconds,
        index_bytes=index_bytes,
        vector_count=info.base_count,
        dimension=info.dimension,
        details={
            "index": args.es_index,
            "version": version,
            "index_type": "bbq_hnsw",
            "metric": "l2_norm",
            "m": args.m,
            "ef_construction": args.ef_construction,
            "shards": 1,
            "replicas": 0,
            "force_merged": not args.es_skip_force_merge,
            "segment_count": segment_count,
            "bulk_client_threads": args.build_threads,
            "bulk_queue_size": args.es_bulk_queue_size,
            "server_threads": "controlled by Elasticsearch node CPU allocation",
            "source_enabled": False,
            "store_stability_samples": store_stats_samples,
            "store_stabilization_seconds": store_stabilization_seconds,
            "store_stabilization_included_in_total_seconds": False,
            "storage_scope": "primary shard store including retained raw vectors",
        },
    )


SearchCallable = Callable[[np.ndarray, int], list[int]]


def run_accuracy(
    search: SearchCallable,
    queries: np.ndarray,
    ground_truth: np.ndarray,
    search_value: int,
    topk: int,
    threads: int,
) -> tuple[float, float]:
    labels = np.empty((queries.shape[0], topk), dtype=np.int64)

    def search_one(item: tuple[int, np.ndarray]) -> tuple[int, list[int]]:
        query_id, vector = item
        return query_id, search(vector, search_value)

    with ThreadPoolExecutor(max_workers=threads) as executor:
        for query_id, result in executor.map(search_one, enumerate(queries)):
            if len(result) != topk:
                raise RuntimeError(
                    f"query {query_id} returned {len(result)} results, expected {topk}"
                )
            labels[query_id] = result
    return compute_recall(labels, ground_truth, topk)


def warm_up(
    search: SearchCallable,
    queries: np.ndarray,
    search_value: int,
    threads: int,
    warmup_queries: int,
) -> None:
    if warmup_queries == 0:
        return

    def run(query_id: int) -> None:
        search(queries[query_id % queries.shape[0]], search_value)

    with ThreadPoolExecutor(max_workers=threads) as executor:
        list(executor.map(run, range(warmup_queries)))


def measure_qps(
    search: SearchCallable,
    queries: np.ndarray,
    search_value: int,
    threads: int,
    duration_seconds: float,
) -> tuple[float, int, float]:
    barrier = threading.Barrier(threads + 1)
    start = threading.Event()
    stop = threading.Event()

    def worker(worker_id: int) -> int:
        query_id = worker_id % queries.shape[0]
        count = 0
        barrier.wait()
        start.wait()
        while not stop.is_set():
            search(queries[query_id], search_value)
            query_id = (query_id + threads) % queries.shape[0]
            count += 1
        return count

    with ThreadPoolExecutor(max_workers=threads) as executor:
        futures = [executor.submit(worker, thread_id) for thread_id in range(threads)]
        barrier.wait()
        started = time.perf_counter()
        start.set()
        stop.wait(duration_seconds)
        stop.set()
        completed = sum(future.result() for future in futures)
        elapsed = time.perf_counter() - started
    return completed / elapsed, completed, elapsed


def benchmark_search(
    engine: str,
    search_value_name: str,
    search: SearchCallable,
    queries: np.ndarray,
    ground_truth: np.ndarray,
    info: DatasetInfo,
    args: argparse.Namespace,
    details: dict[str, Any],
) -> list[SearchResult]:
    results: list[SearchResult] = []
    for threads in args.search_threads:
        for search_value in args.search_values:
            recall_at_1, recall_at_k = run_accuracy(
                search,
                queries,
                ground_truth,
                search_value,
                args.topk,
                threads,
            )
            warm_up(
                search,
                queries,
                search_value,
                threads,
                args.warmup_queries,
            )
            measurements = [
                measure_qps(
                    search,
                    queries,
                    search_value,
                    threads,
                    args.duration_seconds,
                )
                for _ in range(args.repeats)
            ]
            qps_repeats = [measurement[0] for measurement in measurements]
            result = SearchResult(
                engine=engine,
                search_value_name=search_value_name,
                search_value=search_value,
                threads=threads,
                query_count=info.query_count,
                topk=args.topk,
                qps=float(statistics.median(qps_repeats)),
                qps_repeats=qps_repeats,
                throughput_query_counts=[
                    measurement[1] for measurement in measurements
                ],
                throughput_seconds=[measurement[2] for measurement in measurements],
                recall_at_1=recall_at_1,
                recall_at_k=recall_at_k,
                details=details,
            )
            results.append(result)
            print(
                f"{engine:13s} threads={threads:2d} "
                f"{search_value_name}={search_value:4d} "
                f"qps={result.qps:10.2f} recall@{args.topk}={recall_at_k:.4f}"
            )
    return results


def zvec_search_callable(collection: Any, zvec: Any, topk: int) -> SearchCallable:
    def search(vector: np.ndarray, ef: int) -> list[int]:
        docs = collection.query(
            zvec.Query(
                field_name=VECTOR_FIELD,
                vector=vector,
                param=zvec.HnswRabitqQueryParam(ef=ef),
            ),
            topk=topk,
        )
        return [int(doc.id) for doc in docs]

    return search


def elasticsearch_search_callable(
    client: Any, args: argparse.Namespace
) -> SearchCallable:
    def search(vector: np.ndarray, num_candidates: int) -> list[int]:
        knn: dict[str, Any] = {
            "field": VECTOR_FIELD,
            "query_vector": vector.tolist(),
            "k": args.topk,
            "num_candidates": num_candidates,
        }
        if args.es_rescore_oversample is not None:
            knn["rescore_vector"] = {
                "oversample": args.es_rescore_oversample,
            }
        response = client.search(
            index=args.es_index,
            knn=knn,
            size=args.topk,
            source=False,
        )
        return [int(hit["_id"]) for hit in response["hits"]["hits"]]

    return search


def append_search_results(work_dir: Path, results: Sequence[SearchResult]) -> None:
    with (work_dir / RESULTS_NAME).open("a", encoding="utf-8") as output:
        for result in results:
            output.write(json.dumps(asdict(result), sort_keys=True) + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dataset",
        required=True,
        type=Path,
        help="ANN-Benchmarks HDF5 file with train/test/neighbors datasets.",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=Path("/tmp/zvec-hnsw-rabitq-vs-es"),
    )
    parser.add_argument(
        "--mode",
        choices=("inspect", "build", "search", "all"),
        default="inspect",
    )
    parser.add_argument(
        "--engines",
        choices=("both", "zvec", "elasticsearch"),
        default="both",
    )
    parser.add_argument("--m", type=parse_positive_int, default=16)
    parser.add_argument("--ef-construction", type=parse_positive_int, default=100)
    parser.add_argument("--build-threads", type=parse_positive_int, default=16)
    parser.add_argument("--topk", type=parse_positive_int, default=10)
    parser.add_argument(
        "--search-values",
        type=parse_int_list,
        default=parse_int_list("32,64,128,256,512,1000"),
        help="Zvec ef and Elasticsearch num_candidates sweep.",
    )
    parser.add_argument(
        "--search-threads",
        type=parse_int_list,
        default=parse_int_list("1,4,8,16"),
    )
    parser.add_argument("--duration-seconds", type=parse_positive_float, default=10.0)
    parser.add_argument("--repeats", type=parse_positive_int, default=3)
    parser.add_argument(
        "--warmup-queries",
        type=parse_nonnegative_int,
        default=1000,
    )
    parser.add_argument(
        "--max-base",
        type=parse_nonnegative_int,
        default=0,
        help="Smoke-test base limit; 0 uses all vectors.",
    )
    parser.add_argument(
        "--max-queries",
        type=parse_nonnegative_int,
        default=0,
        help="Query limit; 0 uses all queries.",
    )
    parser.add_argument(
        "--allow-dataset-relocation",
        action="store_true",
        help=(
            "Allow a persisted build to use a dataset copy with a different "
            "path or mtime; SHA-256, size, and ANN metadata must still match."
        ),
    )
    parser.add_argument("--overwrite", action="store_true")

    parser.add_argument("--zvec-total-bits", type=parse_positive_int, default=7)
    parser.add_argument("--zvec-num-clusters", type=parse_positive_int, default=16)
    parser.add_argument(
        "--zvec-sample-count",
        type=parse_nonnegative_int,
        default=0,
    )
    parser.add_argument("--zvec-batch-size", type=parse_positive_int, default=1000)

    parser.add_argument(
        "--es-url", default=os.getenv("ES_URL", "http://localhost:9200")
    )
    parser.add_argument(
        "--es-index",
        default="gist-hnsw-bbq",
    )
    parser.add_argument("--es-api-key", default=os.getenv("ES_API_KEY"))
    parser.add_argument("--es-username", default=os.getenv("ES_USERNAME"))
    parser.add_argument("--es-password", default=os.getenv("ES_PASSWORD"))
    parser.add_argument("--es-insecure", action="store_true")
    parser.add_argument("--es-bulk-docs", type=parse_positive_int, default=500)
    parser.add_argument(
        "--es-bulk-queue-size",
        type=parse_positive_int,
        default=4,
    )
    parser.add_argument(
        "--es-bulk-bytes",
        type=parse_positive_int,
        default=16 * 1024 * 1024,
    )
    parser.add_argument(
        "--es-source-batch-rows",
        type=parse_positive_int,
        default=1000,
    )
    parser.add_argument(
        "--es-request-timeout",
        type=parse_positive_float,
        default=120.0,
    )
    parser.add_argument(
        "--es-force-merge-timeout",
        type=parse_positive_float,
        default=7200.0,
    )
    parser.add_argument(
        "--es-store-stability-timeout",
        type=parse_positive_float,
        default=120.0,
    )
    parser.add_argument("--es-skip-force-merge", action="store_true")
    parser.add_argument(
        "--es-rescore-oversample",
        type=parse_optional_positive_float,
        default=3.0,
        help=(
            "Raw-vector rescore oversampling; use 'none' for a version-pinned "
            "no-rescore experiment."
        ),
    )
    return parser


def print_dataset_info(info: DatasetInfo) -> None:
    print(json.dumps({"dataset": asdict(info)}, indent=2, sort_keys=True))


def initialize_engines(
    args: argparse.Namespace, engines: Sequence[str]
) -> EngineClients:
    clients = EngineClients()
    if "zvec" in engines:
        clients.zvec = initialize_zvec(args)
    if "elasticsearch" in engines:
        (
            clients.elasticsearch,
            clients.elasticsearch_helpers,
            clients.elasticsearch_version,
        ) = create_es_client(args)
    return clients


def run_build_phase(
    args: argparse.Namespace,
    info: DatasetInfo,
    engines: Sequence[str],
    clients: EngineClients,
    h5py: Any,
) -> None:
    manifest = load_manifest(args.work_dir)
    validate_retained_builds(manifest, info, args, engines)
    build_results: list[BuildResult] = []
    with h5py.File(args.dataset, "r") as source:
        if "zvec" in engines:
            build_results.append(
                build_zvec(source, args.work_dir, info, args, clients.zvec)
            )
        if "elasticsearch" in engines:
            build_results.append(
                build_elasticsearch(
                    source,
                    info,
                    args,
                    clients.elasticsearch,
                    clients.elasticsearch_helpers,
                    clients.elasticsearch_version,
                )
            )
    manifest["dataset"] = asdict(info)
    manifest_config = manifest.setdefault("config", {})
    manifest_config.update(
        {
            "m": args.m,
            "ef_construction": args.ef_construction,
            "build_threads": args.build_threads,
        }
    )
    if "zvec" in engines:
        manifest_config.update(
            {
                "zvec_total_bits": args.zvec_total_bits,
                "zvec_num_clusters": args.zvec_num_clusters,
                "zvec_sample_count": args.zvec_sample_count,
            }
        )
    manifest["environment"] = {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
    }
    for result in build_results:
        manifest.setdefault("build", {})[result.engine] = asdict(result)
        print(json.dumps({"build": asdict(result)}, indent=2, sort_keys=True))
    write_json(args.work_dir / MANIFEST_NAME, manifest)


def load_search_data(
    args: argparse.Namespace,
    info: DatasetInfo,
    full_info: DatasetInfo,
    h5py: Any,
) -> tuple[np.ndarray, np.ndarray]:
    with h5py.File(args.dataset, "r") as source:
        queries = dataset_slice(source["test"], 0, info.query_count)
        if info.base_count == full_info.base_count:
            ground_truth = np.ascontiguousarray(
                source["neighbors"][: info.query_count, : args.topk],
                dtype=np.int64,
            )
        else:
            ground_truth = exact_ground_truth(
                source["train"],
                queries,
                info.base_count,
                args.topk,
            )
    validate_ground_truth(ground_truth, info.base_count, args.topk)
    return queries, ground_truth


def run_search_phase(
    args: argparse.Namespace,
    info: DatasetInfo,
    full_info: DatasetInfo,
    engines: Sequence[str],
    clients: EngineClients,
    h5py: Any,
) -> None:
    manifest = load_manifest(args.work_dir)
    validate_manifest(manifest, info, args)
    missing = [engine for engine in engines if engine not in manifest.get("build", {})]
    if missing:
        raise RuntimeError(f"manifest has no build record for engines: {missing}")
    queries, ground_truth = load_search_data(args, info, full_info, h5py)

    if "zvec" in engines:
        collection = clients.zvec.open(
            str(args.work_dir / "zvec"),
            clients.zvec.CollectionOption(read_only=True, enable_mmap=True),
        )
        results = benchmark_search(
            "zvec",
            "ef",
            zvec_search_callable(collection, clients.zvec, args.topk),
            queries,
            ground_truth,
            info,
            args,
            {
                "total_bits": args.zvec_total_bits,
                "raw_vector_rescore": False,
            },
        )
        append_search_results(args.work_dir, results)
        del collection
        gc.collect()

    if "elasticsearch" in engines:
        validate_es_index(clients.elasticsearch, args.es_index, info, args)
        index_bytes, doc_count, segment_count = es_primary_stats(
            clients.elasticsearch, args.es_index
        )
        if doc_count != info.base_count:
            raise RuntimeError(
                f"Elasticsearch reports {doc_count} documents, "
                f"expected {info.base_count}"
            )
        results = benchmark_search(
            "elasticsearch",
            "num_candidates",
            elasticsearch_search_callable(clients.elasticsearch, args),
            queries,
            ground_truth,
            info,
            args,
            {
                "version": clients.elasticsearch_version,
                "rescore_oversample": args.es_rescore_oversample,
                "segment_count": segment_count,
                "index_bytes": index_bytes,
            },
        )
        append_search_results(args.work_dir, results)


def main() -> int:
    args = build_parser().parse_args()
    full_info = inspect_dataset(args.dataset)
    info = selected_dataset_info(full_info, args.max_base, args.max_queries)
    print_dataset_info(info)
    if args.mode == "inspect":
        return 0

    validate_search_args(info, args)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    h5py = import_h5py()
    engines = selected_engines(args.engines)
    clients = initialize_engines(args, engines)
    if args.mode in ("build", "all"):
        run_build_phase(args, info, engines, clients, h5py)
    if args.mode in ("search", "all"):
        run_search_phase(args, info, full_info, engines, clients, h5py)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
