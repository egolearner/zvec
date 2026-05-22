#!/usr/bin/env python3
"""Benchmark tool: Sparse Vector (BM25) build and search performance.

Usage:
    # Build
    python bench_fts_sparse.py build \
        --corpus /path/to/corpus.jsonl \
        --mode sparse --db-dir /tmp/bench_fts_sparse

    # Search
    python bench_fts_sparse.py search \
        --queries /path/to/queries.jsonl \
        --qrels /path/to/qrels/ \
        --mode sparse --db-dir /tmp/bench_fts_sparse --topk 10
"""

from __future__ import annotations

import argparse
import json
import math
import os
import pickle
import shutil
import time
from http import HTTPStatus
from pathlib import Path

import _zvec
from _zvec import _Collection
from _zvec.schema import _CollectionSchema, _FieldSchema

import zvec
from zvec import (
    CollectionOption,
    CollectionSchema,
    DataType,
    Doc,
    Fts,
    FtsIndexParam,
    HnswIndexParam,
    MetricType,
    OptimizeOption,
    Query,
    VectorSchema,
)
from zvec.extension import BM25EmbeddingFunction
from zvec.model import Collection

myprint = print

myprint(_zvec.__file__)


# =============================================================================
# Data Loading
# =============================================================================


def load_corpus(path: str) -> list[dict]:
    """Load corpus.jsonl -> list of {_id, title, text}."""
    docs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                docs.append(json.loads(line))
    return docs


def load_queries(path: str) -> list[dict]:
    """Load queries.jsonl -> list of {_id, text}."""
    queries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                queries.append(json.loads(line))
    return queries


def load_qrels(qrels_dir: str) -> dict[str, set[str]]:
    """Load all TSV files in qrels dir -> {query_id: set(corpus_ids)}."""
    qrels = {}
    qrels_path = Path(qrels_dir)
    for tsv_file in sorted(qrels_path.glob("*.tsv")):
        with open(tsv_file, encoding="utf-8") as f:
            f.readline()  # skip header
            for line in f:
                parts = line.strip().split("\t")
                if len(parts) >= 3:
                    query_id = parts[0]
                    corpus_id = parts[1]
                    score = int(parts[2])
                    if score > 0:
                        if query_id not in qrels:
                            qrels[query_id] = set()
                        qrels[query_id].add(corpus_id)
    return qrels


# =============================================================================
# Metrics
# =============================================================================


def compute_recall_at_k(
    ranked_docs: list[str], relevant: set[str], cutoff: int
) -> float:
    """Compute Recall@cutoff = |retrieved_top_cutoff ∩ relevant| / |relevant|."""
    if not relevant:
        return 0.0
    hits = sum(1 for doc_id in ranked_docs[:cutoff] if doc_id in relevant)
    return hits / len(relevant)


def compute_metrics(
    results_map: dict[str, list[str]], qrels: dict[str, set[str]], topk: int
) -> dict:
    """Compute Recall@1/5/10/K, MRR, NDCG@k over all queries with relevance judgments."""
    recall_cutoffs = [1, 5, 10]
    if topk not in recall_cutoffs:
        recall_cutoffs.append(topk)
    recall_cutoffs = sorted(set(c for c in recall_cutoffs if c <= topk))
    # Always include topk even if > 10
    if topk not in recall_cutoffs:
        recall_cutoffs.append(topk)

    recall_accum: dict[int, list[float]] = {c: [] for c in recall_cutoffs}
    mrrs = []
    ndcgs = []

    for qid, ranked_docs in results_map.items():
        relevant = qrels.get(qid, set())
        if not relevant:
            continue

        # Recall@cutoff for each cutoff
        for cutoff in recall_cutoffs:
            recall_accum[cutoff].append(
                compute_recall_at_k(ranked_docs, relevant, cutoff)
            )

        # MRR
        rr = 0.0
        for rank, doc_id in enumerate(ranked_docs[:topk], start=1):
            if doc_id in relevant:
                rr = 1.0 / rank
                break
        mrrs.append(rr)

        # NDCG@k (binary relevance)
        dcg = 0.0
        for rank, doc_id in enumerate(ranked_docs[:topk], start=1):
            if doc_id in relevant:
                dcg += 1.0 / math.log2(rank + 1)
        ideal_hits = min(len(relevant), topk)
        idcg = sum(1.0 / math.log2(r + 1) for r in range(1, ideal_hits + 1))
        ndcgs.append(dcg / idcg if idcg > 0 else 0.0)

    recall_results = {}
    for cutoff in recall_cutoffs:
        values = recall_accum[cutoff]
        recall_results[cutoff] = sum(values) / len(values) if values else 0.0

    return {
        "num_queries": len(results_map),
        "num_evaluated": len(mrrs),
        "recall_at": recall_results,
        "recall_cutoffs": recall_cutoffs,
        "recall": recall_results.get(topk, 0.0),
        "mrr": sum(mrrs) / len(mrrs) if mrrs else 0.0,
        "ndcg": sum(ndcgs) / len(ndcgs) if ndcgs else 0.0,
    }


# =============================================================================
# Model-based Sparse Encoder (DashScope TextEmbedding)
# =============================================================================


def _sparse_cache_path(source_path: str, model: str, text_type: str) -> str:
    """Cache file path for model-generated sparse vectors, next to source_path."""
    src = Path(source_path)
    safe_model = model.replace("/", "_")
    return str(src.with_name(f"{src.name}.sparse-{safe_model}-{text_type}.pkl"))


def encode_sparse_model_cached(
    source_path: str,
    texts: list[str],
    model: str,
    text_type: str,
    batch_size: int,
    label: str = "Encode",
    concurrency: int = 8,
) -> tuple[list[dict[int, float]], float, bool]:
    """Encode texts via model with on-disk cache next to source_path.

    Returns (vectors, gen_time_seconds, cache_hit). ``gen_time`` is the
    summed per-thread API time (matching encode_sparse_model). On cache hit
    the original recorded value is returned so stats stay comparable.
    """
    cache_path = _sparse_cache_path(source_path, model, text_type)
    if os.path.exists(cache_path):
        try:
            with open(cache_path, "rb") as f:
                cached = pickle.load(f)
            if (
                cached.get("model") == model
                and cached.get("text_type") == text_type
                and cached.get("count") == len(texts)
            ):
                vectors = cached["vectors"]
                gen_time = float(cached.get("gen_time", 0.0))
                myprint(
                    f"  [Cache hit] {len(vectors)} sparse vectors from "
                    f"{cache_path} (orig gen time: {gen_time:.2f}s)"
                )
                return vectors, gen_time, True
            myprint(
                f"  [Cache] {cache_path} does not match "
                f"(model/text_type/count); regenerating."
            )
        except Exception as e:
            myprint(f"  [Cache] Failed to load {cache_path}: {e}; regenerating.")

    vectors, gen_time = encode_sparse_model(
        texts, model, text_type, batch_size, label, concurrency=concurrency
    )

    try:
        with open(cache_path, "wb") as f:
            pickle.dump(
                {
                    "model": model,
                    "text_type": text_type,
                    "count": len(texts),
                    "gen_time": gen_time,
                    "vectors": vectors,
                },
                f,
            )
        myprint(f"  [Cache] Saved sparse vectors to {cache_path}")
    except Exception as e:
        myprint(f"  [Cache] Failed to save {cache_path}: {e}")

    return vectors, gen_time, False


# HTTP status codes for which a retry is worthwhile (rate-limit + transient
# server / gateway failures). Other 4xx are client errors and should fail fast.
_RETRYABLE_STATUS = {429, 500, 502, 503, 504}


def encode_sparse_model(
    texts: list[str],
    model: str,
    text_type: str,
    batch_size: int,
    label: str = "Encode",
    concurrency: int = 8,
    max_retries: int = 8,
    retry_base_delay: float = 1.0,
    retry_max_delay: float = 30.0,
    max_input_chars: int = 8192,
) -> tuple[list[dict[int, float]], float]:
    """Encode texts to sparse vectors via DashScope TextEmbedding.

    Dispatches batches across a thread pool of size ``concurrency``. Each
    worker times its own API call; the returned ``gen_time`` is the sum of
    those per-worker durations (total API work time, independent of how
    parallel it ran). Time spent sleeping between retries is included so
    rate-limit waits are accounted for.

    Transient failures (HTTP 429 / 5xx, network exceptions) are retried up
    to ``max_retries`` times with exponential backoff + jitter, capped at
    ``retry_max_delay`` seconds. Non-retryable failures raise immediately.

    Returns (vectors, summed_thread_time). ``vectors`` is aligned with the
    input order — each batch writes to a disjoint slice of the output list,
    so the merge is correctness-safe.
    """
    import random
    from concurrent.futures import ThreadPoolExecutor, as_completed
    from threading import Lock

    from dashscope import TextEmbedding

    vectors: list[dict[int, float]] = [None] * len(texts)  # type: ignore[list-item]
    total = len(texts)

    # Proactive per-text clip to the model's input cap (text-embedding-v4
    # accepts up to 8192). Done up front so all batches see clipped input;
    # the reactive truncate-on-400 path below remains as defense in depth
    # for scripts where chars < tokens.
    if max_input_chars and max_input_chars > 0:
        orig_lens = [len(t) for t in texts]
        clipped_idx = [i for i, L in enumerate(orig_lens) if L > max_input_chars]
        if clipped_idx:
            max_orig = max(orig_lens[i] for i in clipped_idx)
            myprint(
                f"  [{label}] pre-truncated {len(clipped_idx)}/{total} text(s) "
                f"to {max_input_chars} chars (max original={max_orig})"
            )
            texts = [
                t[:max_input_chars] if len(t) > max_input_chars else t
                for t in texts
            ]

    batches: list[tuple[int, list[str]]] = [
        (i, texts[i : i + batch_size]) for i in range(0, total, batch_size)
    ]
    workers = max(1, concurrency)

    summed_thread_time = 0.0
    done = 0
    completed_batches = 0
    retry_count = 0
    truncate_count = 0
    lock = Lock()

    # Reactive truncation fallback: kicks in only if proactive clipping
    # wasn't enough (e.g. CJK where tokens > chars). Halves from the
    # already-clipped size each time.
    truncate_initial = max(1, (max_input_chars or 8192) // 2)
    max_truncate_attempts = 8

    def _is_length_error(resp) -> bool:
        msg = (getattr(resp, "message", "") or "").lower()
        return (
            resp.status_code == 400
            and ("length" in msg or "too long" in msg)
        )

    def _call_with_retry(batch: list[str]) -> object:
        nonlocal retry_count, truncate_count
        last_err: str | None = None
        current_batch = list(batch)
        truncate_limit: int | None = None
        truncate_attempts = 0

        for attempt in range(max_retries + 1):
            try:
                resp = TextEmbedding.call(
                    model=model,
                    input=current_batch,
                    output_type="sparse",
                    text_type=text_type,
                )
            except Exception as e:
                # Network / SDK-level exception — treat as transient.
                if attempt >= max_retries:
                    raise RuntimeError(
                        f"DashScope API exception after {attempt} retries: {e!r}"
                    ) from e
                delay = min(
                    retry_base_delay * (2 ** attempt), retry_max_delay
                ) + random.uniform(0, 0.5)
                with lock:
                    retry_count += 1
                myprint(
                    f"\n  [retry] exception={e!r}; "
                    f"sleep {delay:.1f}s (attempt {attempt + 1}/{max_retries})"
                )
                time.sleep(delay)
                continue

            if resp.status_code == HTTPStatus.OK:
                return resp

            # Server says input is too long — truncate per-text and retry
            # immediately (no backoff sleep, this isn't a transient fault).
            if _is_length_error(resp) and truncate_attempts < max_truncate_attempts:
                if truncate_limit is None:
                    truncate_limit = truncate_initial
                else:
                    truncate_limit = max(1, truncate_limit // 2)
                current_batch = [t[:truncate_limit] for t in current_batch]
                truncate_attempts += 1
                with lock:
                    truncate_count += 1
                myprint(
                    f"\n  [truncate] status=400 code={resp.code!r} "
                    f"req={resp.request_id!r}; clip each text to "
                    f"{truncate_limit} chars and retry"
                )
                continue

            if resp.status_code in _RETRYABLE_STATUS and attempt < max_retries:
                delay = min(
                    retry_base_delay * (2 ** attempt), retry_max_delay
                ) + random.uniform(0, 0.5)
                with lock:
                    retry_count += 1
                myprint(
                    f"\n  [retry] status={resp.status_code} code={resp.code!r} "
                    f"req={resp.request_id!r}; sleep {delay:.1f}s "
                    f"(attempt {attempt + 1}/{max_retries})"
                )
                time.sleep(delay)
                last_err = (
                    f"status={resp.status_code} code={resp.code!r} "
                    f"message={resp.message!r} request_id={resp.request_id!r}"
                )
                continue

            raise RuntimeError(
                f"DashScope API error (status={resp.status_code}, "
                f"code={resp.code!r}, message={resp.message!r}, "
                f"request_id={resp.request_id!r})"
            )

        raise RuntimeError(
            f"DashScope API retries exhausted ({max_retries}); last error: {last_err}"
        )

    def work(start: int, batch: list[str]) -> tuple[list[tuple[int, dict[int, float]]], float]:
        t0 = time.perf_counter()
        resp = _call_with_retry(batch)
        items: list[tuple[int, dict[int, float]]] = []
        for emb in resp.output["embeddings"]:
            local_idx = int(emb.get("text_index", 0))
            sparse_dict = {
                int(item["index"]): float(item["value"])
                for item in emb.get("sparse_embedding", [])
            }
            # Guard against empty vectors (zvec sparse insert may reject empty).
            if not sparse_dict:
                sparse_dict = {0: 0.0}
            items.append((start + local_idx, sparse_dict))
        return items, time.perf_counter() - t0

    t_wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = [ex.submit(work, start, batch) for start, batch in batches]
        for fut in as_completed(futures):
            items, elapsed = fut.result()
            for idx, vec in items:
                vectors[idx] = vec
            with lock:
                summed_thread_time += elapsed
                done += len(items)
                completed_batches += 1
                if completed_batches % 5 == 0 or done == total:
                    myprint(
                        f"\r  [{label}] {done}/{total} encoded    ",
                        end="",
                        flush=True,
                    )
    t_wall = time.perf_counter() - t_wall_start
    myprint()
    myprint(
        f"  [{label}] workers={workers}  "
        f"summed_thread_time={summed_thread_time:.2f}s  "
        f"wall_time={t_wall:.2f}s  retries={retry_count}  "
        f"truncates={truncate_count}"
    )
    return vectors, summed_thread_time


# =============================================================================
# Build
# =============================================================================


def build_fts(
    corpus: list[dict],
    db_dir: str,
    batch_size: int,
    tokenizer: str = "standard",
    filters: list[str] | None = None,
    extra_params: str = "",
) -> None:
    """Build FTS collection using native full-text search index."""
    fts_dir = os.path.join(db_dir, "fts")
    if os.path.exists(fts_dir):
        shutil.rmtree(fts_dir)

    if filters is None:
        filters = ["lowercase"]

    fts_index_param = FtsIndexParam(
        tokenizer_name=tokenizer,
        filters=filters,
        extra_params=extra_params,
    )
    myprint(
        f"[FTS Build] FtsIndexParam: tokenizer={tokenizer}, filters={filters}, extra_params={extra_params!r}"
    )

    # FTS field (STRING + FtsIndexParam) is a forward field at C++ level;
    # zvec requires at least one vector field, so we add a dummy sparse vector.
    fts_field = _FieldSchema(
        name="content",
        data_type=DataType.STRING,
        dimension=0,
        nullable=False,
        index_param=fts_index_param,
    )
    dummy_vec = _FieldSchema(
        name="_vec",
        data_type=DataType.SPARSE_VECTOR_FP32,
        dimension=0,
        nullable=False,
        index_param=HnswIndexParam(metric_type=MetricType.IP),
    )
    schema_obj = _CollectionSchema(name="fts_bench", fields=[fts_field, dummy_vec])

    myprint(f"[FTS Build] Creating collection at {fts_dir}")
    coll = _Collection.CreateAndOpen(fts_dir, schema_obj, CollectionOption())
    collection = Collection._from_core(coll)

    total = len(corpus)
    t_start = time.perf_counter()

    for i in range(0, total, batch_size):
        batch = corpus[i : i + batch_size]
        docs = []
        for item in batch:
            content = (item.get("title") or "") + " " + (item.get("text") or "")
            docs.append(
                Doc(
                    id=str(item["_id"]),
                    fields={"content": content.strip()},
                    vectors={"_vec": {0: 0.0}},
                )
            )
        results = collection.insert(docs)
        inserted = sum(1 for r in results if r.ok())
        if (i // batch_size) % 5 == 0:
            myprint(
                f"\r  Inserted {min(i + batch_size, total)}/{total} ({inserted}/{len(batch)} ok)    ",
                end="",
                flush=True,
            )

    myprint()  # newline after progress
    t_insert = time.perf_counter() - t_start

    myprint("[FTS Build] Optimizing...")
    t_opt_start = time.perf_counter()
    collection.optimize(option=OptimizeOption())
    t_opt = time.perf_counter() - t_opt_start

    t_total = time.perf_counter() - t_start
    myprint("[FTS Build] Done.")
    myprint(f"  Insert:       {t_insert:.2f}s")
    myprint(f"  Optimize:     {t_opt:.2f}s")
    myprint(f"  Total:        {t_total:.2f}s")
    myprint(f"  Throughput:   {total / t_total:.1f} docs/s")
    myprint(f"  Doc count:    {collection.stats.doc_count}")


def build_sparse(
    corpus: list[dict],
    db_dir: str,
    batch_size: int,
    language: str,
    encoder: str = "corpus",
    model_name: str = "text-embedding-v4",
    embed_batch_size: int = 10,
    embed_concurrency: int = 8,
    corpus_path: str | None = None,
) -> None:
    """Build Sparse Vector collection.

    encoder:
        - "corpus":  BM25 with IDF fit on this corpus
        - "builtin": BM25 with dashtext builtin IDF
        - "model":   DashScope sparse embedding model (e.g. text-embedding-v4),
                     vectors pre-generated; generation time is reported
                     separately and NOT counted toward build time.
    """
    sparse_dir = os.path.join(db_dir, "sparse")
    if os.path.exists(sparse_dir):
        shutil.rmtree(sparse_dir)

    schema = CollectionSchema(
        name="sparse_bench",
        vectors=[
            VectorSchema(
                name="sparse",
                data_type=DataType.SPARSE_VECTOR_FP32,
                index_param=HnswIndexParam(metric_type=MetricType.IP),
            ),
        ],
    )

    myprint(f"[Sparse Build] Creating collection at {sparse_dir} (encoder={encoder})")
    collection = zvec.create_and_open(sparse_dir, schema)

    # Prepare corpus texts
    corpus_texts = []
    for item in corpus:
        content = (item.get("title") or "") + " " + (item.get("text") or "")
        corpus_texts.append(content.strip())

    total = len(corpus)

    # ---------------- Encoder init ----------------
    t_enc_init = 0.0
    if encoder == "model":
        # Model encoder has no local init; the API client is initialized
        # lazily inside encode_sparse_model.
        pass
    elif encoder == "builtin":
        myprint(
            f"[Sparse Build] Initializing BM25 builtin encoder (lang={language})..."
        )
        t_enc_init_start = time.perf_counter()
        bm25_doc = BM25EmbeddingFunction(
            language=language,
            encoding_type="document",
        )
        t_enc_init = time.perf_counter() - t_enc_init_start
        myprint(f"  Encoder init: {t_enc_init:.2f}s")
    else:  # "corpus"
        myprint(
            f"[Sparse Build] Initializing BM25 corpus encoder (corpus={total}, lang={language})..."
        )
        t_enc_init_start = time.perf_counter()
        bm25_doc = BM25EmbeddingFunction(
            corpus=corpus_texts,
            encoding_type="document",
            language=language,
        )
        t_enc_init = time.perf_counter() - t_enc_init_start
        myprint(f"  Encoder init: {t_enc_init:.2f}s")

    # ---------------- Pre-generate vectors (timed separately) ----------------
    if encoder == "model":
        myprint(
            f"[Sparse Build] Pre-encoding {total} docs with model={model_name} "
            f"(text_type=document, api_batch={embed_batch_size}, "
            f"concurrency={embed_concurrency})..."
        )
        if corpus_path is None:
            raise ValueError("corpus_path is required for model encoder caching")
        pre_encoded, t_gen, _cache_hit = encode_sparse_model_cached(
            source_path=corpus_path,
            texts=corpus_texts,
            model=model_name,
            text_type="document",
            batch_size=embed_batch_size,
            label="Doc encode",
            concurrency=embed_concurrency,
        )
    else:
        myprint(f"[Sparse Build] Pre-encoding {total} docs with BM25 ({encoder})...")
        t_gen_start = time.perf_counter()
        pre_encoded = [None] * total  # type: ignore[list-item]
        for i, text in enumerate(corpus_texts):
            pre_encoded[i] = bm25_doc.embed(text)
            if (i + 1) % 1000 == 0:
                myprint(
                    f"\r  [Doc encode] {i + 1}/{total} encoded    ", end="", flush=True
                )
        myprint()
        t_gen = time.perf_counter() - t_gen_start
    gen_throughput = total / t_gen if t_gen > 0 else 0
    myprint(f"  Vector gen:   {t_gen:.2f}s ({gen_throughput:.1f} docs/s)  [separate]")

    # ---------------- Insert (vector generation excluded) ----------------
    t_insert_start = time.perf_counter()
    for i in range(0, total, batch_size):
        batch_items = corpus[i : i + batch_size]
        docs = []
        for idx, item in enumerate(batch_items):
            docs.append(
                Doc(
                    id=str(item["_id"]),
                    vectors={"sparse": pre_encoded[i + idx]},
                )
            )
        results = collection.insert(docs)
        inserted = sum(1 for r in results if r.ok())
        if (i // batch_size) % 5 == 0:
            myprint(
                f"\r  Inserted {min(i + batch_size, total)}/{total} ({inserted}/{len(batch_items)} ok)    ",
                end="",
                flush=True,
            )

    myprint()  # newline after progress
    t_insert = time.perf_counter() - t_insert_start

    # ---------------- Optimize ----------------
    myprint("[Sparse Build] Optimizing...")
    t_opt_start = time.perf_counter()
    collection.optimize(option=OptimizeOption())
    t_opt = time.perf_counter() - t_opt_start

    # Build total: encoder init + insert + optimize. Excludes vector
    # generation time, which is reported separately for all encoders.
    t_build = t_enc_init + t_insert + t_opt

    # Save metadata for query phase
    meta: dict = {"encoder": encoder}
    if encoder == "corpus":
        meta["corpus_texts"] = corpus_texts
    elif encoder == "model":
        meta["model_name"] = model_name
        meta["embed_batch_size"] = embed_batch_size
    pickle_path = os.path.join(db_dir, "sparse_meta.pkl")
    with open(pickle_path, "wb") as f:
        pickle.dump(meta, f)

    build_throughput = total / t_build if t_build > 0 else 0
    myprint(f"[Sparse Build] Done. (encoder={encoder})")
    myprint(f"  Vector gen:   {t_gen:.2f}s ({gen_throughput:.1f} docs/s)  [separate]")
    myprint(f"  Encoder init: {t_enc_init:.2f}s")
    myprint(f"  Insert:       {t_insert:.2f}s")
    myprint(f"  Optimize:     {t_opt:.2f}s")
    myprint(f"  Build total:  {t_build:.2f}s  (excl. vector gen)")
    myprint(f"  Throughput:   {build_throughput:.1f} docs/s")
    myprint(f"  Doc count:    {collection.stats.doc_count}")


# =============================================================================
# Search
# =============================================================================


def search_fts(
    queries: list[dict], qrels: dict[str, set[str]], db_dir: str, topk: int
) -> None:
    """Run FTS search and report metrics."""
    fts_dir = os.path.join(db_dir, "fts")
    coll = _Collection.Open(fts_dir, CollectionOption(read_only=True))
    collection = Collection._from_core(coll)

    results_map = {}
    latencies = []

    total = len(queries)
    for i, q in enumerate(queries):
        qid = str(q["_id"])
        text = q["text"]

        t0 = time.perf_counter()
        results = collection.query(
            queries=Query(field_name="content", fts=Fts(match_string=text)),
            topk=topk,
        )
        latency = time.perf_counter() - t0
        latencies.append(latency)

        results_map[qid] = [doc.id for doc in results]

        if (i + 1) % 200 == 0:
            myprint(f"\r  {i + 1}/{total} queries done    ", end="", flush=True)

    myprint()  # newline after progress
    # Compute metrics
    metrics = compute_metrics(results_map, qrels, topk)
    total_time = sum(latencies)
    avg_latency = total_time / len(latencies) if latencies else 0
    qps = len(latencies) / total_time if total_time > 0 else 0

    myprint(f"\n{'=' * 55}")
    myprint(f"  [FTS] Search Results (top-{topk})")
    myprint(f"{'=' * 55}")
    myprint(f"  Queries total:     {metrics['num_queries']}")
    myprint(f"  Queries evaluated: {metrics['num_evaluated']}")
    myprint(f"  Avg latency:       {avg_latency * 1000:.2f} ms")
    myprint(f"  QPS:               {qps:.1f}")
    for cutoff in metrics["recall_cutoffs"]:
        myprint(f"  Recall@{cutoff:<10} {metrics['recall_at'][cutoff]:.4f}")
    myprint(f"  MRR:               {metrics['mrr']:.4f}")
    myprint(f"  NDCG@{topk}:         {metrics['ndcg']:.4f}")
    myprint(f"{'=' * 55}\n")


def search_sparse(
    queries: list[dict],
    qrels: dict[str, set[str]],
    db_dir: str,
    topk: int,
    language: str,
    queries_path: str | None = None,
    embed_concurrency: int = 8,
) -> None:
    """Run sparse vector search and report metrics."""
    sparse_dir = os.path.join(db_dir, "sparse")

    # Load build metadata to determine encoder type
    meta_path = os.path.join(db_dir, "sparse_meta.pkl")
    old_pickle_path = os.path.join(db_dir, "sparse_corpus.pkl")
    if os.path.exists(meta_path):
        with open(meta_path, "rb") as f:
            meta = pickle.load(f)
        if "encoder" in meta:
            encoder = meta["encoder"]
        elif meta.get("use_builtin"):
            encoder = "builtin"
        else:
            encoder = "corpus"
    elif os.path.exists(old_pickle_path):
        # Backward compat: old build saved corpus texts directly
        with open(old_pickle_path, "rb") as f:
            corpus_texts_legacy = pickle.load(f)
        meta = {"encoder": "corpus", "corpus_texts": corpus_texts_legacy}
        encoder = "corpus"
    else:
        meta = {"encoder": "builtin"}
        encoder = "builtin"

    myprint(f"[Sparse Search] encoder={encoder}")

    bm25_query = None
    t_init = 0.0

    # ---------------- Encoder init ----------------
    if encoder == "model":
        model_name = meta.get("model_name", "text-embedding-v4")
        embed_batch_size = int(meta.get("embed_batch_size", 10))
    elif encoder == "builtin":
        myprint(f"[Sparse Search] Init BM25 builtin query encoder (lang={language})...")
        t_init_start = time.perf_counter()
        bm25_query = BM25EmbeddingFunction(
            language=language,
            encoding_type="query",
        )
        t_init = time.perf_counter() - t_init_start
        myprint(f"  Encoder init: {t_init:.2f}s")
    else:  # "corpus"
        corpus_texts = meta.get("corpus_texts", [])
        myprint(
            f"[Sparse Search] Init BM25 corpus query encoder (corpus={len(corpus_texts)})..."
        )
        t_init_start = time.perf_counter()
        bm25_query = BM25EmbeddingFunction(
            corpus=corpus_texts,
            encoding_type="query",
            language=language,
        )
        t_init = time.perf_counter() - t_init_start
        myprint(f"  Encoder init: {t_init:.2f}s")

    # ---------------- Pre-generate query vectors (timed separately) ----------------
    query_texts = [q["text"] for q in queries]
    total = len(queries)
    if encoder == "model":
        myprint(
            f"[Sparse Search] Pre-encoding {total} queries with "
            f"model={model_name} (text_type=query, api_batch={embed_batch_size}, "
            f"concurrency={embed_concurrency})..."
        )
        if queries_path is None:
            raise ValueError("queries_path is required for model encoder caching")
        pre_encoded_queries, t_gen, _cache_hit = encode_sparse_model_cached(
            source_path=queries_path,
            texts=query_texts,
            model=model_name,
            text_type="query",
            batch_size=embed_batch_size,
            label="Query encode",
            concurrency=embed_concurrency,
        )
    else:
        myprint(
            f"[Sparse Search] Pre-encoding {total} queries with BM25 ({encoder})..."
        )
        t_gen_start = time.perf_counter()
        pre_encoded_queries = [bm25_query.embed(t) for t in query_texts]
        t_gen = time.perf_counter() - t_gen_start
    gen_throughput = total / t_gen if t_gen > 0 else 0
    avg_gen_ms = (t_gen / total * 1000) if total else 0
    myprint(
        f"  Vector gen:   {t_gen:.2f}s ({gen_throughput:.1f} queries/s, avg {avg_gen_ms:.2f} ms/query)  [separate]"
    )

    collection = zvec.open(sparse_dir, CollectionOption(read_only=True))

    results_map = {}
    latencies = []

    for i, q in enumerate(queries):
        qid = str(q["_id"])
        sparse_vec = pre_encoded_queries[i]

        t0 = time.perf_counter()
        results = collection.query(
            queries=Query(field_name="sparse", vector=sparse_vec),
            topk=topk,
        )
        latency = time.perf_counter() - t0
        latencies.append(latency)

        results_map[qid] = [doc.id for doc in results]

        if (i + 1) % 200 == 0:
            myprint(f"\r  {i + 1}/{total} queries done    ", end="", flush=True)

    myprint()  # newline after progress
    # Compute metrics
    metrics = compute_metrics(results_map, qrels, topk)
    total_time = sum(latencies)
    avg_latency = total_time / len(latencies) if latencies else 0
    qps = len(latencies) / total_time if total_time > 0 else 0

    myprint(f"\n{'=' * 55}")
    myprint(f"  [SPARSE ({encoder})] Search Results (top-{topk})")
    myprint(f"{'=' * 55}")
    # Combined latency/QPS that includes per-query share of vector generation.
    combined_total_time = total_time + t_gen
    combined_avg_latency = combined_total_time / len(latencies) if latencies else 0
    combined_qps = (
        len(latencies) / combined_total_time if combined_total_time > 0 else 0
    )

    myprint(f"  Queries total:     {metrics['num_queries']}")
    myprint(f"  Queries evaluated: {metrics['num_evaluated']}")
    myprint(f"  Vector gen total:  {t_gen:.2f}s (avg {avg_gen_ms:.2f} ms/query)")
    myprint(
        f"  Avg latency:       {avg_latency * 1000:.2f} ms   QPS: {qps:.1f}    (search only, excl. vector gen)"
    )
    myprint(
        f"  Avg latency:       {combined_avg_latency * 1000:.2f} ms   QPS: {combined_qps:.1f}    (incl. vector gen)"
    )
    for cutoff in metrics["recall_cutoffs"]:
        myprint(f"  Recall@{cutoff:<10} {metrics['recall_at'][cutoff]:.4f}")
    myprint(f"  MRR:               {metrics['mrr']:.4f}")
    myprint(f"  NDCG@{topk}:         {metrics['ndcg']:.4f}")
    myprint(f"{'=' * 55}\n")


# =============================================================================
# CLI
# =============================================================================


def cmd_build(args: argparse.Namespace) -> None:
    """Execute build command."""
    zvec.init()
    corpus = load_corpus(args.corpus)
    myprint(f"Loaded {len(corpus)} documents from {args.corpus}")
    os.makedirs(args.db_dir, exist_ok=True)

    if args.mode in ("fts", "both"):
        fts_filters = [f.strip() for f in args.fts_filters.split(",") if f.strip()]
        build_fts(
            corpus,
            args.db_dir,
            args.batch_size,
            tokenizer=args.fts_tokenizer,
            filters=fts_filters,
            extra_params=args.fts_extra_params,
        )
        myprint()

    if args.mode in ("sparse", "both"):
        build_sparse(
            corpus,
            args.db_dir,
            args.batch_size,
            args.language,
            encoder=args.sparse_encoder,
            model_name=args.model_name,
            embed_batch_size=args.embed_batch_size,
            embed_concurrency=args.embed_concurrency,
            corpus_path=args.corpus,
        )
        myprint()


def cmd_search(args: argparse.Namespace) -> None:
    """Execute search command."""
    zvec.init()
    queries = load_queries(args.queries)
    qrels = load_qrels(args.qrels)
    myprint(f"Loaded {len(queries)} queries, {len(qrels)} with relevance judgments")

    if args.mode in ("fts", "both"):
        myprint("\n[FTS Search] Starting...")
        search_fts(queries, qrels, args.db_dir, args.topk)

    if args.mode in ("sparse", "both"):
        myprint("\n[Sparse Search] Starting...")
        search_sparse(
            queries,
            qrels,
            args.db_dir,
            args.topk,
            args.language,
            queries_path=args.queries,
            embed_concurrency=args.embed_concurrency,
        )


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark FTS vs Sparse Vector (BM25) search in zvec"
    )
    subparsers = parser.add_subparsers(dest="command", help="Sub-commands")

    # Build
    bp = subparsers.add_parser("build", help="Build collection(s)")
    bp.add_argument("--corpus", required=True, help="Path to corpus.jsonl")
    bp.add_argument(
        "--mode", choices=["fts", "sparse", "both"], default="both", help="Build mode"
    )
    bp.add_argument("--db-dir", required=True, help="Database directory")
    bp.add_argument("--language", default="en", help="Language (default: en)")
    bp.add_argument(
        "--batch-size", type=int, default=500, help="Batch size (default: 500)"
    )
    bp.add_argument(
        "--sparse-encoder",
        choices=["corpus", "builtin", "model"],
        default="corpus",
        help="Sparse encoder: 'corpus' BM25 with corpus-fitted IDF, 'builtin' BM25 with dashtext IDF, 'model' DashScope sparse embedding model (default: corpus)",
    )
    bp.add_argument(
        "--model-name",
        default="text-embedding-v4",
        help="DashScope model name when --sparse-encoder=model (default: text-embedding-v4). Requires DASHSCOPE_API_KEY env var.",
    )
    bp.add_argument(
        "--embed-batch-size",
        type=int,
        default=10,
        help="Per-call input batch size for the embedding API (default: 10)",
    )
    bp.add_argument(
        "--embed-concurrency",
        type=int,
        default=8,
        help="Number of concurrent embedding API workers (default: 8)",
    )
    bp.add_argument(
        "--fts-tokenizer",
        default="standard",
        help="FTS tokenizer name (default: standard)",
    )
    bp.add_argument(
        "--fts-filters",
        default="lowercase",
        help="FTS filters, comma-separated (default: lowercase)",
    )
    bp.add_argument(
        "--fts-extra-params",
        default="",
        help="FTS extra_params string (default: empty)",
    )

    # Search
    sp = subparsers.add_parser("search", help="Search and evaluate")
    sp.add_argument("--queries", required=True, help="Path to queries.jsonl")
    sp.add_argument("--qrels", required=True, help="Path to qrels directory")
    sp.add_argument(
        "--mode", choices=["fts", "sparse", "both"], default="both", help="Search mode"
    )
    sp.add_argument("--db-dir", required=True, help="Database directory")
    sp.add_argument("--topk", type=int, default=10, help="Top-k (default: 10)")
    sp.add_argument("--language", default="en", help="Language (default: en)")
    sp.add_argument(
        "--embed-concurrency",
        type=int,
        default=8,
        help="Number of concurrent embedding API workers (default: 8)",
    )

    args = parser.parse_args()
    if args.command == "build":
        cmd_build(args)
    elif args.command == "search":
        cmd_search(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
