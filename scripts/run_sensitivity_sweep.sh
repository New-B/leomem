#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT_DIR="${ROOT}/results/sensitivity"
BASE_CFG="${ROOT}/configs/leomem_full_4node.conf"
YCSB_BENCH="${BUILD_DIR}/benchmarks/leomem_workload_ycsb_mixed"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "${OUT_DIR}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

run_case() {
  local key="$1"
  local value="$2"
  local workload="$3"
  local cfg="${TMP_DIR}/${key}_${value}_${workload}.conf"
  cp "${BASE_CFG}" "${cfg}"
  printf '\n%s=%s\n' "${key}" "${value}" >> "${cfg}"
  "${YCSB_BENCH}" "${cfg}" "${workload}" 20000 4096 > "${OUT_DIR}/${key}_${value}_${workload}.txt"
}

for value in 4 8 16; do
  run_case profiling_window_size "${value}" A
done

for value in 1 2 4; do
  run_case cache_admission_min_reads "${value}" A
done

for value in 1 2 4; do
  run_case remote_write_batch_threshold "${value}" A
done

for value in 1 2 4; do
  run_case adaptive_mode_batch_threshold "${value}" A
done

python3 "${ROOT}/scripts/collect_results.py" "${OUT_DIR}" > "${OUT_DIR}/summary.csv"
printf 'sensitivity results written to %s\n' "${OUT_DIR}"
