#!/usr/bin/env bash
# Host: unpack m5 writefile archives, reconstruct run dirs, verify using SPEC.
set -euo pipefail

usage() {
  cat <<'HELP'
Usage: 03_verify_host.sh --group DIR --prepared DIR --spec-dir DIR [--output DIR] [--keep-work]
Example:
  ./03_verify_host.sh --group ./m5_test_grup_1 \
    --prepared ./spec-runs/ref --spec-dir /opt/spec2017

Group layout:
  m5_test_grup_1/m5_out_test1/stats.txt
  m5_test_grup_1/m5_out_test1/spec_519.lbm_r_run01.tar.gz
  m5_test_grup_1/m5_out_test2/spec_502.gcc_r_run01.tar.gz

Unpacked archives remain under group/verified/expanded; compare logs and
summary.tsv under group/verified. By default temporary full run copies are
removed after comparison to conserve disk space.
HELP
}

GROUP=""; PREPARED=""; SPEC_DIR=""; OUTPUT=""; KEEP_WORK=0
while (( $# )); do
  case "$1" in
    --group) GROUP=${2:?}; shift 2 ;;
    --prepared) PREPARED=${2:?}; shift 2 ;;
    --spec-dir) SPEC_DIR=${2:?}; shift 2 ;;
    --output) OUTPUT=${2:?}; shift 2 ;;
    --keep-work) KEEP_WORK=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$GROUP" && -n "$PREPARED" && -n "$SPEC_DIR" ]] || { usage >&2; exit 2; }
GROUP=$(realpath "$GROUP")
PREPARED=$(realpath "$PREPARED")
SPEC_DIR=$(realpath "$SPEC_DIR")
[[ -f "$SPEC_DIR/shrc" ]] || { echo "Missing $SPEC_DIR/shrc" >&2; exit 1; }
# shellcheck disable=SC1090
source "$SPEC_DIR/shrc"
command -v specinvoke >/dev/null || { echo 'specinvoke not available' >&2; exit 1; }
[[ -n "$OUTPUT" ]] || OUTPUT="$GROUP/verified"
mkdir -p "$OUTPUT"
OUTPUT=$(realpath "$OUTPUT")
mkdir -p "$OUTPUT/expanded" "$OUTPUT/reports" "$OUTPUT/work"
SUMMARY="$OUTPUT/summary.tsv"
printf 'gem5_out\tbenchmark\trun_id\tbenchmark_rc\tm5_dump_rc\tcompare_rc\tmisfiles\tresult\tarchive\n' > "$SUMMARY"

shopt -s nullglob
archives=("$GROUP"/m5_out_test*/spec_*.tar.gz)
(( ${#archives[@]} > 0 )) || { echo "No spec_*.tar.gz in m5_out_test* under $GROUP" >&2; exit 1; }
any_failed=0
for archive in "${archives[@]}"; do
  out_name=$(basename "$(dirname "$archive")")
  base_name=$(basename "$archive" .tar.gz)
  dest="$OUTPUT/expanded/$out_name/$base_name"
  report="$OUTPUT/reports/$out_name/$base_name"
  mkdir -p "$dest" "$report"
  echo "== $out_name / $base_name =="

  # Hash file is emitted alongside the archive by 02_run_guest.sh.
  if [[ -f "$archive.sha256" ]]; then
    if ! ( cd "$(dirname "$archive")" && sha256sum -c "$(basename "$archive").sha256" ); then
      echo "ERROR: checksum mismatch: $archive" >&2
      any_failed=1
      continue
    fi
  else
    echo "WARNING: no SHA256 file for $archive" >&2
  fi
  tar -xzf "$archive" -C "$dest" --no-same-owner
  meta="$dest/metadata.txt"
  if [[ ! -f "$meta" ]]; then
    echo "Invalid archive (no metadata.txt): $archive" >&2
    any_failed=1
    continue
  fi
  field() { sed -n "s/^$1=//p" "$meta" | head -1; }
  format=$(field format); bench=$(field test); run_name=$(field run_dir)
  run_id=$(field run_id); benchmark_rc=$(field exit_code); m5_dump_rc=$(field m5_dump_status)
  if [[ "$format" != spec-gem5-v1 || ! "$bench" =~ ^[0-9]{3}\.[A-Za-z0-9_]+$ || \
        ! "$run_name" =~ ^run_base_[A-Za-z0-9_.-]+$ || \
        ! "$run_id" =~ ^[A-Za-z0-9_-]+$ || ! "$benchmark_rc" =~ ^[0-9]+$ || ! "$m5_dump_rc" =~ ^[0-9]+$ ]]; then
    echo "Invalid metadata: $meta" >&2
    any_failed=1
    continue
  fi
  original="$PREPARED/$bench/$run_name"
  [[ -d "$original" && -f "$original/compare.cmd" ]] || {
    echo "Prepared run dir missing: $original" >&2
    any_failed=1
    continue
  }
  work="$OUTPUT/work/$out_name/$base_name/$bench/$run_name"
  mkdir -p "$work"
  # Use CoW reflinks when the host FS supports them. Never modify the source.
  cp -a --reflink=auto "$original/." "$work/"
  if [[ -d "$dest/run_delta" ]]; then
    cp -a --reflink=auto "$dest/run_delta/." "$work/"
  fi
  # Apply deletions from the guest run (paths are relative to the run dir).
  if [[ -f "$dest/deleted.txt" ]]; then
    while IFS= read -r rel; do
      [[ -n "$rel" ]] || continue
      [[ "$rel" != /* && "$rel" != ../* && "$rel" != *'/../'* ]] || {
        echo "Unsafe deleted path ignored: $rel" >&2; continue;
      }
      rm -f -- "$work/$rel"
    done < "$dest/deleted.txt"
  fi

  compare_rc=0
  (cd "$work" && specinvoke -f compare.cmd) > "$report/compare.log" 2>&1 || compare_rc=$?
  # Some specinvoke versions print an aggregated command status in the log.
  if grep -Eq 'specinvoke exit: rc=[1-9][0-9]*' "$report/compare.log"; then
    compare_rc=1
  fi
  find "$work" -type f -name '*.mis' -print > "$report/miscompare_files.txt"
  misfiles=$(wc -l < "$report/miscompare_files.txt" | tr -d ' ')
  # Keep diagnostic compare-generated *.cmp, compare.err etc. in the report.
  find "$work" -maxdepth 1 -type f \
    \( -name '*.cmp' -o -name 'compare.err' -o -name 'compare.stdout' -o -name '*.mis' \) \
    -exec cp -a -- '{}' "$report/" \;

  result='PASS'
  if (( benchmark_rc != 0 || m5_dump_rc != 0 || compare_rc != 0 || misfiles != 0 )); then
    result='FAIL'; any_failed=1
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$out_name" "$bench" "$run_id" "$benchmark_rc" "$m5_dump_rc" "$compare_rc" \
    "$misfiles" "$result" "$archive" >> "$SUMMARY"
  echo "$bench: $result (benchmark rc=$benchmark_rc, comparison rc=$compare_rc, *.mis=$misfiles)"
  if (( !KEEP_WORK )); then
    rm -rf -- "$OUTPUT/work/$out_name/$base_name"
  fi
done
printf '\nSummary: %s\n' "$SUMMARY"
if (( any_failed )); then
  echo 'One or more benchmarks failed to run or verify.' >&2
  exit 1
fi
