#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT_DIR="${ROOT}/results"
ITER_BENCH="${BUILD_DIR}/benchmarks/leomem_workload_iterative_analytics"
YCSB_BENCH="${BUILD_DIR}/benchmarks/leomem_workload_ycsb_mixed"
KMEANS_BENCH="${BUILD_DIR}/benchmarks/leomem_workload_kmeans"
PAGERANK_BENCH="${BUILD_DIR}/benchmarks/leomem_workload_pagerank"

mkdir -p "${OUT_DIR}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

configs=(
  "configs/leomem_full_4node.conf"
  "configs/baseline_always_cache.conf"
  "configs/baseline_fixed_si.conf"
  "configs/baseline_fixed_wi.conf"
  "configs/ablation_no_batching.conf"
)

for cfg in "${configs[@]}"; do
  name="$(basename "${cfg}" .conf)"
  "${ITER_BENCH}" "${ROOT}/${cfg}" > "${OUT_DIR}/${name}_iterative.txt"
  "${YCSB_BENCH}" "${ROOT}/${cfg}" A 20000 4096 > "${OUT_DIR}/${name}_ycsb_A.txt"
  "${YCSB_BENCH}" "${ROOT}/${cfg}" B 20000 4096 > "${OUT_DIR}/${name}_ycsb_B.txt"
  "${KMEANS_BENCH}" "${ROOT}/${cfg}" 4096 8 4 4 > "${OUT_DIR}/${name}_kmeans.txt"
  "${PAGERANK_BENCH}" "${ROOT}/${cfg}" 2048 4 4 > "${OUT_DIR}/${name}_pagerank.txt"
done

python3 "${ROOT}/scripts/collect_results.py" "${OUT_DIR}" > "${OUT_DIR}/summary.csv"
printf 'results written to %s\n' "${OUT_DIR}"
