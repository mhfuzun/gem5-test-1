#!/usr/bin/env bash
# Host: prepare or reuse SPEC CPU2017 run directories and export portable copies.
set -euo pipefail

usage() {
  cat <<'HELP'
Usage:
  01_prepare_spec.sh --spec-dir PATH --config NAME --dest PATH \
      [--size ref|test|train] [--export-only] [--resume] [--skip BENCHMARK]... BENCHMARK...

Default: build and runsetup each benchmark, then export it.
--export-only: do not call runcpu; reuse existing run directories.
--resume: retain already exported, complete run folders without recopying.
--skip BENCHMARK: exclude that benchmark; may be repeated.

Failures are recorded without stopping the remaining benchmarks.
Results: DEST/manifest.tsv, DEST/failures.tsv, DEST/skipped.tsv,
         DEST/_preparation_logs/ (if build/runsetup was requested).
Only successful runs are listed in manifest.tsv.
Exit status: 0 if no failures, 1 if any benchmark failed, 2 for bad arguments.
HELP
}

SPEC_DIR=""; CONFIG=""; DEST=""; SIZE="ref"; EXPORT_ONLY=0; RESUME=0
BENCHMARKS=(); SKIP_BENCHMARKS=()
while (( $# )); do
  case "$1" in
    --spec-dir) SPEC_DIR=${2:?}; shift 2 ;;
    --config) CONFIG=${2:?}; shift 2 ;;
    --dest) DEST=${2:?}; shift 2 ;;
    --size) SIZE=${2:?}; shift 2 ;;
    --export-only) EXPORT_ONLY=1; shift ;;
    --resume) RESUME=1; shift ;;
    --skip) SKIP_BENCHMARKS+=("${2:?}"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; BENCHMARKS+=("$@"); break ;;
    -*) echo "Unknown option: $1" >&2; exit 2 ;;
    *) BENCHMARKS+=("$1"); shift ;;
  esac
done

[[ -n "$SPEC_DIR" && -n "$CONFIG" && -n "$DEST" ]] || { usage >&2; exit 2; }
[[ "$SIZE" == ref || "$SIZE" == test || "$SIZE" == train ]] || { echo 'Invalid --size' >&2; exit 2; }
(( ${#BENCHMARKS[@]} > 0 )) || { usage >&2; exit 2; }
for b in "${BENCHMARKS[@]}" "${SKIP_BENCHMARKS[@]}"; do
  [[ "$b" =~ ^[0-9]{3}\.[A-Za-z0-9_]+$ ]] || { echo "Invalid benchmark: $b" >&2; exit 2; }
done

SPEC_DIR=$(realpath "$SPEC_DIR")
mkdir -p "$DEST"
DEST=$(realpath "$DEST")
[[ -f "$SPEC_DIR/shrc" ]] || { echo "Missing $SPEC_DIR/shrc" >&2; exit 1; }

# SPEC shrc locates its root from the current working directory.
pushd "$SPEC_DIR" >/dev/null
set +u
# shellcheck disable=SC1091
source ./shrc
set -u
popd >/dev/null
[[ -n "${SPEC:-}" && "$(realpath "$SPEC")" == "$SPEC_DIR" ]] || {
  echo "SPEC root mismatch: ${SPEC:-UNSET} vs $SPEC_DIR" >&2; exit 1;
}
command -v specinvoke >/dev/null || { echo 'specinvoke not found' >&2; exit 1; }
command -v rsync >/dev/null || { echo 'rsync not found' >&2; exit 1; }
if (( !EXPORT_ONLY )); then
  command -v runcpu >/dev/null || { echo 'runcpu not found' >&2; exit 1; }
fi

printf 'benchmark\trun_dir\n' > "$DEST/manifest.tsv"
printf 'benchmark\tstage\treason\n' > "$DEST/failures.tsv"
printf 'benchmark\treason\n' > "$DEST/skipped.tsv"
mkdir -p "$DEST/_preparation_logs"

passed=0; failed=0; skipped=0; reused=0
record_fail() {
  local bench=$1 stage=$2 reason=$3
  printf '%s\t%s\t%s\n' "$bench" "$stage" "$reason" >> "$DEST/failures.tsv"
  printf 'FAILED %-20s [%s] %s\n' "$bench" "$stage" "$reason" >&2
  failed=$((failed + 1))
}

printf 'SPEC: %s\nDestination: %s\nMode: %s\n' \
  "$SPEC_DIR" "$DEST" "$( (( EXPORT_ONLY )) && echo 'export-only' || echo 'build + runsetup + export')"

for bench in "${BENCHMARKS[@]}"; do
  echo
  echo "== $bench =="
  skip_it=0
  for excluded in "${SKIP_BENCHMARKS[@]}"; do
    if [[ "$bench" == "$excluded" ]]; then skip_it=1; break; fi
  done
  if (( skip_it )); then
    printf '%s\tuser --skip\n' "$bench" >> "$DEST/skipped.tsv"
    skipped=$((skipped + 1))
    echo "SKIPPED $bench (--skip)"
    continue
  fi

  if (( !EXPORT_ONLY )); then
    build_log="$DEST/_preparation_logs/${bench}.build.log"
    if runcpu --config="$CONFIG" --action=build --tune=base --copies=1 \
        "$bench" > "$build_log" 2>&1; then
      echo "Build OK ($build_log)"
    else
      rc=$?
      record_fail "$bench" build "runcpu exit=$rc; see $build_log"
      continue
    fi

    setup_log="$DEST/_preparation_logs/${bench}.runsetup.log"
    if runcpu --config="$CONFIG" --action=runsetup --nobuild --size="$SIZE" \
        --tune=base --copies=1 "$bench" > "$setup_log" 2>&1; then
      echo "Runsetup OK ($setup_log)"
    else
      rc=$?
      record_fail "$bench" runsetup "runcpu exit=$rc; see $setup_log"
      continue
    fi
  fi

  src_parent="$SPEC_DIR/benchspec/CPU/$bench/run"
  if [[ ! -d "$src_parent" ]]; then
    record_fail "$bench" locate "No run directory: $src_parent"
    continue
  fi

  # Prefer the newest matching run directory, as in the original script.
  # A config-specific name is not assumed: --config filename and run label
  # can differ (e.g. spec_risc-v_cfg.cfg vs mytest).
  mapfile -t candidates < <(find "$src_parent" -mindepth 1 -maxdepth 1 -type d \
    -name "run_base_${SIZE}*.0000" -printf '%T@\t%p\n' | sort -rn | cut -f2-)
  if (( ${#candidates[@]} == 0 )); then
    record_fail "$bench" locate "No run_base_${SIZE}*.0000 directory"
    continue
  fi
  if (( ${#candidates[@]} > 1 )); then
    echo "WARNING: more than one run directory; selecting newest: ${candidates[0]}" >&2
  fi
  src_dir=${candidates[0]}
  dir_name=$(basename "$src_dir")
  if [[ ! -f "$src_dir/speccmds.cmd" || ! -f "$src_dir/compare.cmd" ]]; then
    missing=""
    [[ -f "$src_dir/speccmds.cmd" ]] || missing="speccmds.cmd "
    [[ -f "$src_dir/compare.cmd" ]] || missing+="compare.cmd"
    record_fail "$bench" validate "Missing ${missing} in $src_dir"
    continue
  fi

  # Resume: keep previously successful exports, including those prepared by
  # the earlier script invocation. Always rebuild the manifest of valid runs.
  final="$DEST/$bench/$dir_name"
  if (( RESUME )) && [[ -s "$final/run_benchmark.sh" && \
                         -f "$final/speccmds.cmd" && -f "$final/compare.cmd" ]]; then
    printf '%s\t%s\n' "$bench" "$dir_name" >> "$DEST/manifest.tsv"
    reused=$((reused + 1))
    echo "Reused existing export: $final"
    continue
  fi

  # Generate the command script BEFORE replacing an existing export.
  mkdir -p "$DEST/$bench"
  scratch=$(mktemp -d "$DEST/$bench/.export.XXXXXXXX") || {
    record_fail "$bench" export 'Could not create staging directory'
    continue
  }
  dryrun="$scratch/specinvoke_nn.txt"
  if ! (cd "$src_dir" && specinvoke -nn speccmds.cmd) > "$dryrun" 2> "$scratch/specinvoke_error.txt"; then
    record_fail "$bench" specinvoke "specinvoke -nn failed; source: $src_dir"
    rm -rf -- "$scratch"
    continue
  fi
  if ! grep -Eq '^# Starting run for copy #0([[:space:]]|$)' "$dryrun"; then
    record_fail "$bench" specinvoke 'No copy #0 command section found'
    rm -rf -- "$scratch"
    continue
  fi
  if grep -Eq '^# Starting run for copy #[1-9]' "$dryrun"; then
    record_fail "$bench" specinvoke 'Multiple copies not supported'
    rm -rf -- "$scratch"
    continue
  fi

  # Stage all data and the generated runner first. If anything fails, an
  # earlier successful export remains available for inspection.
  if ! rsync -aL "$src_dir/" "$scratch/"; then
    record_fail "$bench" rsync "Failed to copy source: $src_dir"
    rm -rf -- "$scratch"
    continue
  fi
  target="$scratch/run_benchmark.sh"
  cat > "$target" <<'RUNNER'
#!/usr/bin/env bash
# Generated from SPEC specinvoke -nn. Runs in this directory on guest.
set -euo pipefail
cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")"
export OMP_NUM_THREADS=1 OMP_THREAD_LIMIT=1
export LC_ALL=C
RUNNER
  if ! awk '
    /^# Starting run for copy #0([[:space:]]|$)/ { in_commands=1; next }
    /^specinvoke exit:/ { exit }
    in_commands && /^cd[[:space:]]/ { next }
    in_commands && /^# Starting run for copy/ { next }
    in_commands { print; if ($0 !~ /^([[:space:]]*|#.*)$/) count++ }
    END { if (!count) exit 12 }
  ' "$dryrun" >> "$target"; then
    record_fail "$bench" generate 'No runnable command lines from specinvoke'
    rm -rf -- "$scratch"
    continue
  fi
  if ! bash -n "$target"; then
    record_fail "$bench" generate 'Generated runner has invalid bash syntax'
    rm -rf -- "$scratch"
    continue
  fi
  chmod 755 "$target"
  rm -f "$scratch/specinvoke_nn.txt" "$scratch/specinvoke_error.txt"

  # Copy a fully prepared staging directory to the final exported path.
  final="$DEST/$bench/$dir_name"
  mkdir -p "$final"
  if ! rsync -a --delete "$scratch/" "$final/"; then
    record_fail "$bench" export "Final rsync failed: $final"
    rm -rf -- "$scratch"
    continue
  fi
  rm -rf -- "$scratch"
  printf '%s\t%s\n' "$bench" "$dir_name" >> "$DEST/manifest.tsv"
  passed=$((passed + 1))
  printf 'Prepared %-18s %s\n' "$bench" "$final"
done

printf '\n========== PREPARATION SUMMARY ==========\n'
printf 'Prepared: %d | Reused: %d | Failed: %d | Skipped: %d | Requested: %d\n' \
  "$passed" "$reused" "$failed" "$skipped" "${#BENCHMARKS[@]}"
printf 'Manifest: %s\nFailures: %s\nSkipped: %s\n' \
  "$DEST/manifest.tsv" "$DEST/failures.tsv" "$DEST/skipped.tsv"
if (( failed )); then
  echo 'Failed tests:'
  awk -F '\t' 'NR>1 {printf "  %-20s %-12s %s\n",$1,$2,$3}' "$DEST/failures.tsv"
  exit 1
fi
printf 'Ready. You may rsync the exported tree into the guest image.\n'
