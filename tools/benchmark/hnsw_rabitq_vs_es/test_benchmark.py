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

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
from argparse import Namespace
from dataclasses import asdict
from pathlib import Path

import h5py
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

SCRIPT_PATH = Path(__file__).with_name("benchmark.py")
RUN_SCRIPT_PATH = Path(__file__).with_name("run_comparison.sh")
SPEC = importlib.util.spec_from_file_location(
    "hnsw_rabitq_vs_es_benchmark", SCRIPT_PATH
)
assert SPEC is not None
assert SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = benchmark
SPEC.loader.exec_module(benchmark)


def write_dataset(path: Path) -> None:
    with h5py.File(path, "w") as target:
        target.attrs["distance"] = "euclidean"
        target.create_dataset(
            "train",
            data=np.arange(48, dtype=np.float32).reshape(12, 4),
        )
        target.create_dataset(
            "test",
            data=np.arange(8, dtype=np.float32).reshape(2, 4),
        )
        target.create_dataset(
            "neighbors",
            data=np.arange(10, dtype=np.int32).reshape(2, 5),
        )
        target.create_dataset(
            "distances",
            data=np.zeros((2, 5), dtype=np.float32),
        )


def write_cohere_dataset(path: Path) -> None:
    path.mkdir()
    pq.write_table(
        pa.table(
            {
                "id": pa.array([30, 10, 20, 40], type=pa.int64()),
                "emb": pa.array(
                    [
                        [100.0, 10.0],
                        [1.0, 1.0],
                        [0.0, 2.0],
                        [-1.0, 0.0],
                    ],
                    type=pa.large_list(pa.float32()),
                ),
            }
        ),
        path / "shuffle_train.parquet",
        row_group_size=2,
    )
    pq.write_table(
        pa.table(
            {
                "id": pa.array([0, 1], type=pa.int64()),
                "emb": pa.array(
                    [[1.0, 0.0], [0.0, 1.0]],
                    type=pa.large_list(pa.float32()),
                ),
            }
        ),
        path / "test.parquet",
    )
    pq.write_table(
        pa.table(
            {
                "id": pa.array([0, 1], type=pa.int64()),
                "neighbors_id": pa.array(
                    [[30, 10, 20], [20, 10, 30]],
                    type=pa.large_list(pa.int64()),
                ),
            }
        ),
        path / "neighbors.parquet",
    )


def test_inspect_dataset(tmp_path: Path) -> None:
    path = tmp_path / "tiny.hdf5"
    write_dataset(path)

    info = benchmark.inspect_dataset(path)

    assert info.base_count == 12
    assert info.query_count == 2
    assert info.dimension == 4
    assert info.groundtruth_k == 5
    assert info.distance == "euclidean"


def test_inspect_cohere_parquet_dataset(tmp_path: Path) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)

    info = benchmark.inspect_dataset(path)

    assert info.base_count == 4
    assert info.query_count == 2
    assert info.dimension == 2
    assert info.groundtruth_k == 3
    assert info.distance == "cosine"
    assert info.dataset_format == "cohere-parquet"


def test_cohere_reader_preserves_ids_and_computes_cosine_ground_truth(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)
    info = benchmark.inspect_dataset(path)
    source = benchmark.open_dataset(path, info)

    batches = list(source.iter_train_batches(info.base_count, batch_size=2))
    train_ids = np.concatenate([ids for ids, _vectors in batches])
    queries = source.load_queries(info.query_count)[1]
    labels = benchmark.exact_ground_truth(
        source,
        queries,
        info.base_count,
        topk=2,
        distance=info.distance,
        block_size=2,
    )

    np.testing.assert_array_equal(train_ids, [30, 10, 20, 40])
    np.testing.assert_array_equal(labels, [[30, 10], [20, 10]])


def test_cohere_search_data_uses_parquet_ground_truth_ids(tmp_path: Path) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)
    info = benchmark.inspect_dataset(path)
    source = benchmark.open_dataset(path, info)

    queries, ground_truth = benchmark.load_search_data(
        Namespace(topk=2),
        info,
        info,
        source,
    )

    np.testing.assert_array_equal(queries, [[1.0, 0.0], [0.0, 1.0]])
    np.testing.assert_array_equal(ground_truth, [[30, 10], [20, 10]])


def test_cohere_fingerprint_survives_directory_relocation(tmp_path: Path) -> None:
    original = tmp_path / "machine-a" / "cohere_medium_1m"
    original.parent.mkdir()
    write_cohere_dataset(original)
    relocated = tmp_path / "machine-b" / "datasets" / "cohere_medium_1m"
    relocated.parent.mkdir(parents=True)
    shutil.copytree(original, relocated)
    relocated_train = relocated / "shuffle_train.parquet"
    stat = relocated_train.stat()
    os.utime(
        relocated_train,
        ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000),
    )

    original_info = benchmark.inspect_dataset(original)
    relocated_info = benchmark.inspect_dataset(relocated)

    assert relocated_info.path != original_info.path
    assert relocated_info.mtime_ns != original_info.mtime_ns
    assert relocated_info.file_size == original_info.file_size
    assert relocated_info.sha256 == original_info.sha256


def test_cohere_reader_detects_dataset_change_after_open(tmp_path: Path) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)
    info = benchmark.inspect_dataset(path)
    source = benchmark.open_dataset(path, info)
    train_path = path / "shuffle_train.parquet"
    stat = train_path.stat()
    os.utime(
        train_path,
        ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000),
    )

    with pytest.raises(RuntimeError, match="changed while it was being read"):
        source.verify_unchanged()


def test_cohere_reader_rejects_change_after_inspection(tmp_path: Path) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)
    info, inspected_stats = benchmark.inspect_dataset_snapshot(path)
    train_path = path / "shuffle_train.parquet"
    stat = train_path.stat()
    os.utime(
        train_path,
        ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000),
    )

    with pytest.raises(RuntimeError, match="changed after it was inspected"):
        benchmark.open_dataset(path, info, inspected_stats)


def test_inspect_dataset_rejects_non_l2(tmp_path: Path) -> None:
    path = tmp_path / "tiny.hdf5"
    write_dataset(path)
    with h5py.File(path, "r+") as target:
        target.attrs["distance"] = "angular"

    with pytest.raises(RuntimeError, match="Euclidean"):
        benchmark.inspect_dataset(path)


def test_file_sha256_matches_known_digest(tmp_path: Path) -> None:
    path = tmp_path / "dataset.bin"
    path.write_bytes(b"portable dataset")

    assert benchmark.file_sha256(path) == (
        "4d1fc226a58e0686bb1eca605958a62fe766e462714d08d0488f55c83371c51e"
    )


def test_compute_recall_uses_set_overlap() -> None:
    labels = np.array([[0, 2], [5, 4]], dtype=np.int64)
    ground_truth = np.array([[0, 1], [3, 4]], dtype=np.int64)

    recall_at_1, recall_at_2 = benchmark.compute_recall(labels, ground_truth, 2)

    assert recall_at_1 == pytest.approx(0.5)
    assert recall_at_2 == pytest.approx(0.5)


def test_exact_ground_truth_for_truncated_base() -> None:
    train = np.array(
        [
            [0.0, 0.0],
            [1.0, 0.0],
            [2.0, 0.0],
            [10.0, 0.0],
        ],
        dtype=np.float32,
    )
    queries = np.array([[1.1, 0.0], [9.0, 0.0]], dtype=np.float32)

    labels = benchmark.exact_ground_truth(train, queries, 4, 2, block_size=2)

    np.testing.assert_array_equal(labels, [[1, 2], [3, 2]])


def test_es_index_definition_aligns_hnsw_parameters() -> None:
    info = benchmark.DatasetInfo(
        path="/data/gist.hdf5",
        file_size=1,
        mtime_ns=2,
        sha256="dataset-sha256",
        distance="euclidean",
        base_count=1_000_000,
        query_count=1000,
        dimension=960,
        groundtruth_k=100,
    )
    args = Namespace(m=16, ef_construction=100)

    definition = benchmark.es_index_definition(info, args)
    field = definition["mappings"]["properties"][benchmark.VECTOR_FIELD]

    assert definition["settings"]["number_of_shards"] == 1
    assert definition["settings"]["number_of_replicas"] == 0
    assert definition["mappings"]["_source"]["enabled"] is False
    assert field["dims"] == 960
    assert field["similarity"] == "l2_norm"
    assert field["index_options"] == {
        "type": "bbq_hnsw",
        "m": 16,
        "ef_construction": 100,
    }


def test_es_index_definition_uses_cosine_for_cohere() -> None:
    info = benchmark.DatasetInfo(
        path="/data/cohere_medium_1m",
        file_size=1,
        mtime_ns=2,
        sha256="dataset-sha256",
        distance="cosine",
        base_count=1_000_000,
        query_count=1000,
        dimension=768,
        groundtruth_k=1000,
        dataset_format="cohere-parquet",
    )
    args = Namespace(m=16, ef_construction=100)

    definition = benchmark.es_index_definition(info, args)
    field = definition["mappings"]["properties"][benchmark.VECTOR_FIELD]

    assert field["dims"] == 768
    assert field["similarity"] == "cosine"


def test_es_actions_use_cohere_parquet_document_ids(tmp_path: Path) -> None:
    path = tmp_path / "cohere_medium_1m"
    write_cohere_dataset(path)
    info = benchmark.inspect_dataset(path)
    source = benchmark.open_dataset(path, info)

    actions = list(
        benchmark.es_actions(
            source,
            "cohere-bbq",
            info.base_count,
            batch_rows=2,
        )
    )

    assert [action["_id"] for action in actions] == ["30", "10", "20", "40"]
    assert all(action["_index"] == "cohere-bbq" for action in actions)


def test_zvec_metric_type_uses_cosine_for_cohere() -> None:
    class MetricType:
        L2 = object()
        COSINE = object()

    fake_zvec = Namespace(MetricType=MetricType)

    assert benchmark.zvec_metric_type(fake_zvec, "cosine") is MetricType.COSINE


def test_wait_for_stable_es_primary_stats_ignores_transient_store_size(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    samples = iter(
        [
            (205_000_000, 10_000, 1),
            (205_000_000, 10_000, 1),
            (138_000_000, 10_000, 1),
            (138_000_000, 10_000, 1),
            (138_000_000, 10_000, 1),
        ]
    )
    monkeypatch.setattr(
        benchmark,
        "es_primary_stats",
        lambda _client, _index_name: next(samples),
    )
    monkeypatch.setattr(benchmark.time, "sleep", lambda _seconds: None)

    stats, sample_count, _elapsed = benchmark.wait_for_stable_es_primary_stats(
        object(),
        "gist-bbq",
        timeout=1.0,
        interval=0.001,
        stable_samples=3,
    )

    assert stats == (138_000_000, 10_000, 1)
    assert sample_count == 5


@pytest.mark.parametrize(
    ("value", "expected"),
    [("3", 3.0), ("none", None), ("off", None)],
)
def test_parse_optional_positive_float(value: str, expected: float | None) -> None:
    assert benchmark.parse_optional_positive_float(value) == expected


@pytest.mark.parametrize("value", ["1", "10"])
def test_parse_optional_positive_float_rejects_invalid_es_range(value: str) -> None:
    with pytest.raises(
        benchmark.argparse.ArgumentTypeError,
        match=r"greater than 1\.0",
    ):
        benchmark.parse_optional_positive_float(value)


def test_portable_runner_help_documents_machine_specific_paths() -> None:
    result = subprocess.run(
        ["bash", str(RUN_SCRIPT_PATH), "--help"],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0
    assert "--dataset PATH" in result.stdout
    assert "--work-dir PATH" in result.stdout
    assert "--cpus LIST" in result.stdout
    assert "ZVEC_BENCH_DATASET" in result.stdout
    assert "Cohere Parquet directory" in result.stdout


def test_portable_runner_requires_dataset_and_work_dir() -> None:
    result = subprocess.run(
        ["bash", str(RUN_SCRIPT_PATH), "inspect", "--dry-run"],
        check=False,
        capture_output=True,
        text=True,
        env={
            key: value
            for key, value in os.environ.items()
            if key not in ("ZVEC_BENCH_DATASET", "ZVEC_BENCH_WORK_DIR")
        },
    )

    assert result.returncode == 2
    assert "--dataset or ZVEC_BENCH_DATASET is required" in result.stderr


def test_portable_runner_dry_run_uses_explicit_machine_configuration(
    tmp_path: Path,
) -> None:
    dataset = tmp_path / "data set.hdf5"
    dataset.touch()
    work_dir = tmp_path / "benchmark results"
    result = subprocess.run(
        [
            "bash",
            str(RUN_SCRIPT_PATH),
            "all",
            "--dataset",
            str(dataset),
            "--work-dir",
            str(work_dir),
            "--cpus",
            "2,4",
            "--python",
            "/opt/zvec-venv/bin/python",
            "--es-port",
            "29200",
            "--es-index",
            "portable-gist",
            "--allow-dataset-relocation",
            "--overwrite",
            "--dry-run",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert "ES_URL=http://127.0.0.1:29200" in result.stdout
    assert "/opt/zvec-venv/bin/python" in result.stdout
    assert "data\\ set.hdf5" in result.stdout
    assert "--zvec-total-bits 1" in result.stdout
    assert "--es-rescore-oversample none" in result.stdout
    assert "--es-index portable-gist" in result.stdout
    assert "--allow-dataset-relocation" in result.stdout
    assert "docker run" in result.stdout
    assert "--label io.zvec.hnsw-rabitq-vs-es.config=" in result.stdout
    assert "port=29200" in result.stdout
    assert "cpus=2\\,4" in result.stdout
    assert "docker stop" in result.stdout


def test_portable_runner_accepts_directory_dataset(tmp_path: Path) -> None:
    dataset = tmp_path / "cohere_medium_1m"
    dataset.mkdir()
    work_dir = tmp_path / "results"

    result = subprocess.run(
        [
            "bash",
            str(RUN_SCRIPT_PATH),
            "inspect",
            "--dataset",
            str(dataset),
            "--work-dir",
            str(work_dir),
            "--cpus",
            "none",
            "--dry-run",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert str(dataset) in result.stdout


def test_portable_runner_overwrite_resets_previous_search_results(
    tmp_path: Path,
) -> None:
    dataset = tmp_path / "gist.hdf5"
    dataset.touch()
    work_dir = tmp_path / "results"
    work_dir.mkdir()
    previous_results = work_dir / benchmark.RESULTS_NAME
    previous_results.write_text('{"old": true}\n', encoding="utf-8")

    result = subprocess.run(
        [
            "bash",
            str(RUN_SCRIPT_PATH),
            "build",
            "--dataset",
            str(dataset),
            "--work-dir",
            str(work_dir),
            "--cpus",
            "none",
            "--engines",
            "zvec",
            "--overwrite",
            "--dry-run",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert f"rm -- {previous_results}" in result.stdout
    assert previous_results.read_text(encoding="utf-8") == '{"old": true}\n'


def test_portable_runner_isolates_smoke_indexes_from_full_run(
    tmp_path: Path,
) -> None:
    dataset = tmp_path / "gist.hdf5"
    dataset.touch()
    work_dir = tmp_path / "results"
    result = subprocess.run(
        [
            "bash",
            str(RUN_SCRIPT_PATH),
            "smoke",
            "--dataset",
            str(dataset),
            "--work-dir",
            str(work_dir),
            "--cpus",
            "none",
            "--es-index",
            "gist-bbq",
            "--overwrite",
            "--dry-run",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert f"--work-dir {work_dir}/smoke" in result.stdout
    assert "--es-index gist-bbq-smoke" in result.stdout


def test_manifest_allows_explicit_dataset_relocation() -> None:
    saved_info = benchmark.DatasetInfo(
        path="/machine-a/data/gist.hdf5",
        file_size=3_844_648_288,
        mtime_ns=100,
        sha256="same-content",
        distance="euclidean",
        base_count=1_000_000,
        query_count=1000,
        dimension=960,
        groundtruth_k=100,
    )
    relocated_info = benchmark.DatasetInfo(
        path="/machine-b/datasets/gist.hdf5",
        file_size=saved_info.file_size,
        mtime_ns=200,
        sha256=saved_info.sha256,
        distance=saved_info.distance,
        base_count=saved_info.base_count,
        query_count=saved_info.query_count,
        dimension=saved_info.dimension,
        groundtruth_k=saved_info.groundtruth_k,
    )
    manifest = {
        "dataset": asdict(saved_info),
        "config": {"m": 16, "ef_construction": 100},
        "build": {
            "elasticsearch": {
                "details": {"index": "gist-bbq"},
            }
        },
    }
    args = Namespace(
        engines="elasticsearch",
        m=16,
        ef_construction=100,
        es_index="gist-bbq",
        allow_dataset_relocation=True,
    )

    benchmark.validate_manifest(manifest, relocated_info, args)

    args.allow_dataset_relocation = False
    with pytest.raises(RuntimeError, match="path:"):
        benchmark.validate_manifest(manifest, relocated_info, args)

    args.allow_dataset_relocation = True
    manifest["dataset"]["sha256"] = "different-content"
    with pytest.raises(RuntimeError, match="sha256:"):
        benchmark.validate_manifest(manifest, relocated_info, args)


def test_legacy_gist_manifest_defaults_to_hdf5_format() -> None:
    info = benchmark.DatasetInfo(
        path="/data/gist.hdf5",
        file_size=3_844_648_288,
        mtime_ns=100,
        sha256="same-content",
        distance="euclidean",
        base_count=1_000_000,
        query_count=1000,
        dimension=960,
        groundtruth_k=100,
    )
    saved_dataset = asdict(info)
    saved_dataset.pop("dataset_format")
    manifest = {
        "dataset": saved_dataset,
        "config": {"m": 16, "ef_construction": 100},
        "build": {
            "elasticsearch": {
                "details": {"index": "gist-bbq"},
            }
        },
    }
    args = Namespace(
        engines="elasticsearch",
        m=16,
        ef_construction=100,
        es_index="gist-bbq",
        allow_dataset_relocation=False,
    )

    benchmark.validate_manifest(manifest, info, args)
