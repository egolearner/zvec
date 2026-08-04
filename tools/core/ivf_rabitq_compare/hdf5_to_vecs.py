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

import argparse
from pathlib import Path

import h5py
import numpy as np


def write_vecs(dataset, output_path: Path, dtype, batch_rows: int) -> None:
    if len(dataset.shape) != 2:
        raise ValueError(f"{dataset.name} must be a two-dimensional dataset")
    row_count, dimension = dataset.shape
    record_type = np.dtype(
        [("dimension", "<i4"), ("values", dtype, (dimension,))], align=False
    )
    with output_path.open("wb") as output:
        for begin in range(0, row_count, batch_rows):
            end = min(begin + batch_rows, row_count)
            values = np.asarray(dataset[begin:end], dtype=dtype, order="C")
            records = np.empty(end - begin, dtype=record_type)
            records["dimension"] = dimension
            records["values"] = values
            records.tofile(output)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert ANN benchmark HDF5 data to fvecs/ivecs files."
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--base-key", default="train")
    parser.add_argument("--query-key", default="test")
    parser.add_argument("--groundtruth-key", default="neighbors")
    parser.add_argument("--batch-rows", type=int, default=8192)
    args = parser.parse_args()
    if args.batch_rows <= 0:
        parser.error("--batch-rows must be positive")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with h5py.File(args.input, "r") as source:
        write_vecs(
            source[args.base_key],
            args.output_dir / "base.fvecs",
            "<f4",
            args.batch_rows,
        )
        write_vecs(
            source[args.query_key],
            args.output_dir / "query.fvecs",
            "<f4",
            args.batch_rows,
        )
        write_vecs(
            source[args.groundtruth_key],
            args.output_dir / "groundtruth.ivecs",
            "<i4",
            args.batch_rows,
        )


if __name__ == "__main__":
    main()
