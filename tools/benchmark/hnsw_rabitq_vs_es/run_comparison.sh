#!/usr/bin/env bash
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

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCHMARK_SCRIPT="${SCRIPT_DIR}/benchmark.py"

usage() {
  cat <<'EOF'
Usage:
  run_comparison.sh [ACTION] [OPTIONS]

Actions:
  inspect    Validate and print dataset metadata.
  start-es   Start or reuse the configured Elasticsearch Docker container.
  stop-es    Stop the configured Elasticsearch Docker container.
  smoke      Run both engines with 10,000 base vectors and 100 queries.
  build      Build the selected engine indexes.
  search     Search the selected existing indexes.
  all        Inspect, build, and search both engines (default).

Machine-specific options:
  --dataset PATH       ANN-Benchmarks HDF5 file or Cohere Parquet directory.
                       Required; may also be set with ZVEC_BENCH_DATASET.
  --work-dir PATH      Index and result directory. Required; may also be set
                       with ZVEC_BENCH_WORK_DIR.
  --cpus LIST          taskset CPU list, for example 0,2,4,6. Required for
                       managed ES/build/search unless set to "none"; may be set
                       with ZVEC_BENCH_CPUS.
  --python PATH        Python from the prepared Zvec environment.
  --engines VALUE      both, zvec, or elasticsearch (default: both).

Elasticsearch options:
  --es-url URL         Existing/managed Elasticsearch URL.
  --es-port PORT       Host port used by the managed Docker container.
  --es-index NAME      Elasticsearch index name.
  --es-image IMAGE     Elasticsearch Docker image.
  --es-container NAME  Docker container name.
  --es-volume NAME     Docker volume name.
  --es-java-opts TEXT  JVM options (default: -Xms4g -Xmx4g).
  --skip-docker        Do not start or stop Elasticsearch.
  --keep-es-running    Keep a container started by this script running.

Benchmark options:
  --zvec-total-bits N
  --es-rescore-oversample VALUE  Number in (1,10), or "none".
  --build-threads N
  --search-threads LIST
  --search-values LIST
  --duration-seconds N
  --repeats N
  --warmup-queries N
  --allow-dataset-relocation  Ignore dataset path and mtime differences while
                       still validating SHA-256, size, shape, and metadata.
  --overwrite
  --dry-run            Print commands without executing them.
  -h, --help

The defaults form the pure 1-bit comparison:
  Zvec total_bits=1, Elasticsearch BBQ without rescore_vector.
EOF
}

fail_usage() {
  echo "error: $*" >&2
  echo "run with --help for usage" >&2
  exit 2
}

require_value() {
  if [[ $# -lt 2 || -z "${2}" ]]; then
    fail_usage "${1} requires a value"
  fi
}

print_command() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
}

run_command() {
  print_command "$@"
  if [[ "${DRY_RUN}" == "0" ]]; then
    "$@"
  fi
}

ACTION="all"
if [[ $# -gt 0 && "${1}" != -* ]]; then
  ACTION="${1}"
  shift
fi

DATASET="${ZVEC_BENCH_DATASET:-}"
WORK_DIR="${ZVEC_BENCH_WORK_DIR:-}"
CPUS="${ZVEC_BENCH_CPUS:-}"
PYTHON_BIN="${ZVEC_BENCH_PYTHON:-python3}"
ENGINES="${ZVEC_BENCH_ENGINES:-both}"
ES_PORT="${ZVEC_BENCH_ES_PORT:-19200}"
ES_URL="${ZVEC_BENCH_ES_URL:-}"
ES_INDEX="${ZVEC_BENCH_ES_INDEX:-gist-hnsw-bbq}"
ES_IMAGE="${ZVEC_BENCH_ES_IMAGE:-docker.elastic.co/elasticsearch/elasticsearch:8.18.8}"
ES_CONTAINER="${ZVEC_BENCH_ES_CONTAINER:-zvec-es-bbq-benchmark-8-18}"
ES_VOLUME="${ZVEC_BENCH_ES_VOLUME:-zvec-es-bbq-benchmark-data}"
ES_JAVA_OPTS="${ZVEC_BENCH_ES_JAVA_OPTS:--Xms4g -Xmx4g}"
ZVEC_TOTAL_BITS="${ZVEC_BENCH_ZVEC_TOTAL_BITS:-1}"
ES_RESCORE_OVERSAMPLE="${ZVEC_BENCH_ES_RESCORE_OVERSAMPLE:-none}"
BUILD_THREADS="${ZVEC_BENCH_BUILD_THREADS:-16}"
SEARCH_THREADS="${ZVEC_BENCH_SEARCH_THREADS:-1,4,8,16}"
SEARCH_VALUES="${ZVEC_BENCH_SEARCH_VALUES:-32,64,128,256,512,1000}"
DURATION_SECONDS="${ZVEC_BENCH_DURATION_SECONDS:-10}"
REPEATS="${ZVEC_BENCH_REPEATS:-3}"
WARMUP_QUERIES="${ZVEC_BENCH_WARMUP_QUERIES:-1000}"
OVERWRITE=0
DRY_RUN=0
MANAGE_ES=1
KEEP_ES_RUNNING=0
ALLOW_DATASET_RELOCATION=0

while [[ $# -gt 0 ]]; do
  case "${1}" in
    --dataset)
      require_value "$@"
      DATASET="${2}"
      shift 2
      ;;
    --work-dir)
      require_value "$@"
      WORK_DIR="${2}"
      shift 2
      ;;
    --cpus)
      require_value "$@"
      CPUS="${2}"
      shift 2
      ;;
    --python)
      require_value "$@"
      PYTHON_BIN="${2}"
      shift 2
      ;;
    --engines)
      require_value "$@"
      ENGINES="${2}"
      shift 2
      ;;
    --es-url)
      require_value "$@"
      ES_URL="${2}"
      shift 2
      ;;
    --es-port)
      require_value "$@"
      ES_PORT="${2}"
      shift 2
      ;;
    --es-index)
      require_value "$@"
      ES_INDEX="${2}"
      shift 2
      ;;
    --es-image)
      require_value "$@"
      ES_IMAGE="${2}"
      shift 2
      ;;
    --es-container)
      require_value "$@"
      ES_CONTAINER="${2}"
      shift 2
      ;;
    --es-volume)
      require_value "$@"
      ES_VOLUME="${2}"
      shift 2
      ;;
    --es-java-opts)
      require_value "$@"
      ES_JAVA_OPTS="${2}"
      shift 2
      ;;
    --zvec-total-bits)
      require_value "$@"
      ZVEC_TOTAL_BITS="${2}"
      shift 2
      ;;
    --es-rescore-oversample)
      require_value "$@"
      ES_RESCORE_OVERSAMPLE="${2}"
      shift 2
      ;;
    --build-threads)
      require_value "$@"
      BUILD_THREADS="${2}"
      shift 2
      ;;
    --search-threads)
      require_value "$@"
      SEARCH_THREADS="${2}"
      shift 2
      ;;
    --search-values)
      require_value "$@"
      SEARCH_VALUES="${2}"
      shift 2
      ;;
    --duration-seconds)
      require_value "$@"
      DURATION_SECONDS="${2}"
      shift 2
      ;;
    --repeats)
      require_value "$@"
      REPEATS="${2}"
      shift 2
      ;;
    --warmup-queries)
      require_value "$@"
      WARMUP_QUERIES="${2}"
      shift 2
      ;;
    --allow-dataset-relocation)
      ALLOW_DATASET_RELOCATION=1
      shift
      ;;
    --overwrite)
      OVERWRITE=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --skip-docker)
      MANAGE_ES=0
      shift
      ;;
    --keep-es-running)
      KEEP_ES_RUNNING=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail_usage "unknown option: ${1}"
      ;;
  esac
done

case "${ACTION}" in
  inspect|start-es|stop-es|smoke|build|search|all)
    ;;
  *)
    fail_usage "unknown action: ${ACTION}"
    ;;
esac

case "${ENGINES}" in
  both|zvec|elasticsearch)
    ;;
  *)
    fail_usage "--engines must be both, zvec, or elasticsearch"
    ;;
esac

if [[ -z "${ES_URL}" ]]; then
  ES_URL="http://127.0.0.1:${ES_PORT}"
fi
ES_CONFIG_LABEL_KEY="io.zvec.hnsw-rabitq-vs-es.config"
ES_CONFIG_SIGNATURE="image=${ES_IMAGE}|port=${ES_PORT}|volume=${ES_VOLUME}|cpus=${CPUS:-none}|java=${ES_JAVA_OPTS}"

if [[ "${ACTION}" != "start-es" && "${ACTION}" != "stop-es" ]]; then
  if [[ -z "${DATASET}" ]]; then
    fail_usage "--dataset or ZVEC_BENCH_DATASET is required"
  fi
  if [[ -z "${WORK_DIR}" ]]; then
    fail_usage "--work-dir or ZVEC_BENCH_WORK_DIR is required"
  fi
  if [[ ! -f "${DATASET}" && ! -d "${DATASET}" ]]; then
    fail_usage "dataset path does not exist: ${DATASET}"
  fi
fi

case "${ACTION}" in
  start-es|smoke|build|search|all)
    if [[ -z "${CPUS}" ]]; then
      fail_usage "--cpus or ZVEC_BENCH_CPUS is required; use --cpus none to disable pinning"
    fi
    ;;
esac

echo "ACTION=${ACTION}"
echo "DATASET=${DATASET:-<not-required>}"
echo "WORK_DIR=${WORK_DIR:-<not-required>}"
echo "BENCH_CPUS=${CPUS:-<not-required>}"
echo "PYTHON=${PYTHON_BIN}"
echo "ENGINES=${ENGINES}"
echo "ES_URL=${ES_URL}"
echo "ES_INDEX=${ES_INDEX}"
echo "ZVEC_TOTAL_BITS=${ZVEC_TOTAL_BITS}"
echo "ES_RESCORE_OVERSAMPLE=${ES_RESCORE_OVERSAMPLE}"
echo "ALLOW_DATASET_RELOCATION=${ALLOW_DATASET_RELOCATION}"

benchmark_prefix=()
if [[ -n "${CPUS}" && "${CPUS}" != "none" ]]; then
  benchmark_prefix=(taskset -c "${CPUS}")
fi

common_args=(
  --dataset "${DATASET}"
  --work-dir "${WORK_DIR}"
  --m 16
  --ef-construction 100
  --build-threads "${BUILD_THREADS}"
  --search-values "${SEARCH_VALUES}"
  --search-threads "${SEARCH_THREADS}"
  --duration-seconds "${DURATION_SECONDS}"
  --repeats "${REPEATS}"
  --warmup-queries "${WARMUP_QUERIES}"
)
if [[ "${ALLOW_DATASET_RELOCATION}" == "1" ]]; then
  common_args+=(--allow-dataset-relocation)
fi

overwrite_args=()
if [[ "${OVERWRITE}" == "1" ]]; then
  overwrite_args=(--overwrite)
fi

run_benchmark() {
  run_command \
    "${benchmark_prefix[@]}" \
    "${PYTHON_BIN}" \
    "${BENCHMARK_SCRIPT}" \
    "${common_args[@]}" \
    "$@"
}

wants_zvec() {
  [[ "${ENGINES}" == "both" || "${ENGINES}" == "zvec" ]]
}

wants_es() {
  [[ "${ENGINES}" == "both" || "${ENGINES}" == "elasticsearch" ]]
}

ES_STARTED_BY_SCRIPT=0

wait_for_es() {
  local started_at="${SECONDS}"
  while true; do
    if curl --fail --silent --show-error --max-time 10 \
      "${ES_URL}/_cluster/health?wait_for_status=yellow&timeout=5s" >/dev/null
    then
      echo "Elasticsearch is ready: ${ES_URL}"
      return
    fi
    if (( SECONDS - started_at >= 120 )); then
      echo "error: Elasticsearch did not become ready within 120 seconds" >&2
      return 1
    fi
    sleep 2
  done
}

start_es() {
  if [[ "${MANAGE_ES}" == "0" ]]; then
    echo "Docker management disabled; using ${ES_URL}"
    return
  fi
  if [[ "${DRY_RUN}" == "1" ]]; then
    local dry_run_command=(
      docker run --detach
      --name "${ES_CONTAINER}"
      --publish "127.0.0.1:${ES_PORT}:9200"
      --ulimit nofile=65535:65535
      --env discovery.type=single-node
      --env xpack.security.enabled=false
      --env xpack.security.http.ssl.enabled=false
      --env xpack.license.self_generated.type=trial
      --env xpack.ml.enabled=false
      --env ingest.geoip.downloader.enabled=false
      --env "ES_JAVA_OPTS=${ES_JAVA_OPTS}"
      --volume "${ES_VOLUME}:/usr/share/elasticsearch/data"
      --label "${ES_CONFIG_LABEL_KEY}=${ES_CONFIG_SIGNATURE}"
    )
    if [[ -n "${CPUS}" && "${CPUS}" != "none" ]]; then
      dry_run_command+=(
        --entrypoint /usr/bin/taskset
        "${ES_IMAGE}"
        -c "${CPUS}" /bin/tini --
        /usr/local/bin/docker-entrypoint.sh eswrapper
      )
    else
      dry_run_command+=("${ES_IMAGE}")
    fi
    run_command "${dry_run_command[@]}"
    run_command curl --fail --silent --show-error "${ES_URL}/"
    ES_STARTED_BY_SCRIPT=1
    return
  fi

  if docker container inspect "${ES_CONTAINER}" >/dev/null 2>&1; then
    local existing_signature
    existing_signature="$(
      docker inspect \
        --format "{{if .Config.Labels}}{{index .Config.Labels \"${ES_CONFIG_LABEL_KEY}\"}}{{end}}" \
        "${ES_CONTAINER}"
    )"
    if [[ "${existing_signature}" != "${ES_CONFIG_SIGNATURE}" ]]; then
      echo "error: existing container ${ES_CONTAINER} has a different or missing benchmark configuration label" >&2
      echo "expected: ${ES_CONFIG_SIGNATURE}" >&2
      echo "actual:   ${existing_signature:-<missing>}" >&2
      echo "use a new --es-container name, or use --skip-docker with an externally managed Elasticsearch" >&2
      return 1
    fi
    if [[ "$(docker inspect --format '{{.State.Running}}' "${ES_CONTAINER}")" != "true" ]]; then
      run_command docker start "${ES_CONTAINER}"
      ES_STARTED_BY_SCRIPT=1
    else
      echo "Elasticsearch container is already running: ${ES_CONTAINER}"
    fi
  else
    local docker_command=(
      docker run --detach
      --name "${ES_CONTAINER}"
      --publish "127.0.0.1:${ES_PORT}:9200"
      --ulimit nofile=65535:65535
      --env discovery.type=single-node
      --env xpack.security.enabled=false
      --env xpack.security.http.ssl.enabled=false
      --env xpack.license.self_generated.type=trial
      --env xpack.ml.enabled=false
      --env ingest.geoip.downloader.enabled=false
      --env "ES_JAVA_OPTS=${ES_JAVA_OPTS}"
      --volume "${ES_VOLUME}:/usr/share/elasticsearch/data"
      --label "${ES_CONFIG_LABEL_KEY}=${ES_CONFIG_SIGNATURE}"
    )
    if [[ -n "${CPUS}" && "${CPUS}" != "none" ]]; then
      docker_command+=(
        --entrypoint /usr/bin/taskset
        "${ES_IMAGE}"
        -c "${CPUS}" /bin/tini --
        /usr/local/bin/docker-entrypoint.sh eswrapper
      )
    else
      docker_command+=("${ES_IMAGE}")
    fi
    run_command "${docker_command[@]}"
    ES_STARTED_BY_SCRIPT=1
  fi
  wait_for_es
}

stop_es() {
  if [[ "${MANAGE_ES}" == "1" ]]; then
    run_command docker stop --timeout 30 "${ES_CONTAINER}"
  fi
}

cleanup() {
  if [[ "${ES_STARTED_BY_SCRIPT}" == "1" && "${KEEP_ES_RUNNING}" == "0" ]]; then
    stop_es
    ES_STARTED_BY_SCRIPT=0
  fi
}

build_indexes() {
  if [[ "${OVERWRITE}" == "1" && -f "${WORK_DIR}/search-results.jsonl" ]]; then
    run_command rm -- "${WORK_DIR}/search-results.jsonl"
  fi
  if wants_zvec; then
    run_benchmark \
      --mode build \
      --engines zvec \
      "${overwrite_args[@]}" \
      --zvec-total-bits "${ZVEC_TOTAL_BITS}" \
      --zvec-num-clusters 16
  fi
  if wants_es; then
    run_benchmark \
      --mode build \
      --engines elasticsearch \
      "${overwrite_args[@]}" \
      --es-url "${ES_URL}" \
      --es-index "${ES_INDEX}"
  fi
}

search_indexes() {
  if wants_zvec; then
    run_benchmark \
      --mode search \
      --engines zvec \
      --zvec-total-bits "${ZVEC_TOTAL_BITS}" \
      --zvec-num-clusters 16
  fi
  if wants_es; then
    run_benchmark \
      --mode search \
      --engines elasticsearch \
      --es-url "${ES_URL}" \
      --es-index "${ES_INDEX}" \
      --es-rescore-oversample "${ES_RESCORE_OVERSAMPLE}"
  fi
}

run_smoke() {
  WORK_DIR="${WORK_DIR}/smoke"
  ES_INDEX="${ES_INDEX}-smoke"
  SEARCH_VALUES="32,64"
  SEARCH_THREADS="1,4"
  DURATION_SECONDS="1"
  REPEATS="1"
  WARMUP_QUERIES="20"
  common_args=(
    --dataset "${DATASET}"
    --work-dir "${WORK_DIR}"
    --m 16
    --ef-construction 100
    --build-threads "${BUILD_THREADS}"
    --search-values "${SEARCH_VALUES}"
    --search-threads "${SEARCH_THREADS}"
    --duration-seconds "${DURATION_SECONDS}"
    --repeats "${REPEATS}"
    --warmup-queries "${WARMUP_QUERIES}"
    --max-base 10000
    --max-queries 100
  )
  if [[ "${ALLOW_DATASET_RELOCATION}" == "1" ]]; then
    common_args+=(--allow-dataset-relocation)
  fi
  build_indexes
  search_indexes
}

run_all() {
  build_indexes
  search_indexes
}

run_with_es_lifecycle() {
  local operation="${1}"
  if wants_es; then
    trap cleanup EXIT
    start_es
  fi
  "${operation}"
  cleanup
  trap - EXIT
}

case "${ACTION}" in
  inspect)
    run_benchmark --mode inspect
    ;;
  start-es)
    KEEP_ES_RUNNING=1
    start_es
    ;;
  stop-es)
    stop_es
    ;;
  smoke)
    run_with_es_lifecycle run_smoke
    ;;
  build)
    run_with_es_lifecycle build_indexes
    ;;
  search)
    run_with_es_lifecycle search_indexes
    ;;
  all)
    run_benchmark --mode inspect
    run_with_es_lifecycle run_all
    ;;
esac
