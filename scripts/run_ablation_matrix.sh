#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT_DIR="${ROOT}/results"
BENCH="${BUILD_DIR}/benchmarks/leomem_workload_iterative_analytics"

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
  "${BENCH}" "${ROOT}/${cfg}" > "${OUT_DIR}/${name}.txt"
done

python3 "${ROOT}/scripts/collect_results.py" "${OUT_DIR}" > "${OUT_DIR}/summary.csv"
printf 'results written to %s\n' "${OUT_DIR}"
