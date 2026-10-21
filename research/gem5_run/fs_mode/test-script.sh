#!/usr/bin/env bash
# Run pre-staged SPEC CPU2017 benchmarks inside a gem5 RISC-V Linux guest.
# Requires 'runcpu --action=runsetup' to have been completed for each benchmark.
set -uo pipefail

SPEC_DIR="${SPEC_DIR:-/opt/spec2017}"
LABEL="${SPEC_LABEL:-mytest}"
DATASET="${SPEC_DATASET:-test}"
TUNE="${SPEC_TUNE:-base}"
COPY_INDEX="${SPEC_COPY_INDEX:-0000}"
OUTPUT_DIR="${SPEC_OUTPUT_DIR:-$HOME/spec-results}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
USE_M5=1
EXIT_AT_END=0
LIST_FILE=""
BENCHMARKS=()

usage() {
  cat <<'USAGE'
Usage: run_spec_gem5.sh [options] 519.lbm_r 502.gcc_r ...
       run_spec_gem5.sh [options] --list-file /path/benchmarks.txt

Options:
  --spec-dir DIR      SPEC CPU2017 installation (default: /opt/spec2017)
  --label NAME        SPEC config label used by runsetup (default: mytest)
  --dataset NAME      test | train | refrate | refspeed (default: test)
  --tune NAME         base | peak (default: base)
  --copy-index NNNN   run directory suffix (default: 0000)
  --output-dir DIR    Guest directory for result archives
  --run-id ID         Unique experiment name (strongly recommended)
  --list-file FILE    Benchmark names, separated by spaces/commas/newlines
  --no-m5             Test under QEMU: no m5 stats or host export
  --exit-at-end       Call 'm5 exit' after export; current config.py
                      saves a checkpoint and stops on this event
  -h, --help          Show this help

Precondition: the run directories and speccmds.cmd/compare.cmd were
created via runcpu --action=runsetup, preferably beforehand under QEMU.
The benchmark output archive appears in gem5's -d output directory.
USAGE
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 2; }

add_benchmarks() {
  local token item
  local -a split=()
  for token in "$@"; do
    token="${token//,/ }"
    read -r -a split <<< "$token"
    for item in "${split[@]}"; do
      [[ "$item" =~ ^[0-9]{3}\.[A-Za-z0-9_]+$ ]] || die "Invalid benchmark name: $item"
      BENCHMARKS+=("$item")
    done
  done
}

while (($#)); do
  case "$1" in
    --spec-dir|--label|--dataset|--tune|--copy-index|--output-dir|--run-id|--list-file)
      (($# >= 2)) || die "Missing value for $1"
      opt="$1"; val="$2"; shift 2
      case "$opt" in
        --spec-dir) SPEC_DIR="$val";;
        --label) LABEL="$val";;
        --dataset) DATASET="$val";;
        --tune) TUNE="$val";;
        --copy-index) COPY_INDEX="$val";;
        --output-dir) OUTPUT_DIR="$val";;
        --run-id) RUN_ID="$val";;
        --list-file) LIST_FILE="$val";;
      esac
      ;;
    --no-m5) USE_M5=0; shift;;
    --exit-at-end) EXIT_AT_END=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; add_benchmarks "$@"; break;;
    -*) die "Unknown option: $1";;
    *) add_benchmarks "$1"; shift;;
  esac
done

if [[ -n "$LIST_FILE" ]]; then
  [[ -r "$LIST_FILE" ]] || die "Cannot read list file: $LIST_FILE"
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    [[ -n "${line//[[:space:],]/}" ]] || continue
    add_benchmarks "$line"
  done < "$LIST_FILE"
fi

((${#BENCHMARKS[@]} > 0)) || die "Give at least one benchmark"
[[ "$RUN_ID" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "Invalid --run-id"
[[ "$RUN_ID" != *..* ]] || die "Run id cannot contain '..'"
[[ "$LABEL" =~ ^[A-Za-z0-9._+-]+$ ]] || die "Invalid --label"
[[ "$TUNE" == base || "$TUNE" == peak ]] || die "--tune must be base or peak"
[[ "$DATASET" =~ ^(test|train|refrate|refspeed)$ ]] || die "Invalid --dataset"
[[ "$COPY_INDEX" =~ ^[0-9]{4}$ ]] || die "--copy-index must be four digits"
[[ -f "$SPEC_DIR/shrc" ]] || die "Missing $SPEC_DIR/shrc"
[[ "$USE_M5" -eq 0 || -n "$(command -v m5 || true)" ]] || die "m5 not found; install it or use --no-m5 under QEMU"

SPEC_DIR="$(cd "$SPEC_DIR" && pwd -P)" || die "Invalid SPEC directory"
# SPECPATH and toolchain environment required by specinvoke/compare.cmd.
# shellcheck disable=SC1090
source "$SPEC_DIR/shrc" || die "Cannot source $SPEC_DIR/shrc"
command -v specinvoke >/dev/null 2>&1 || die "specinvoke is unavailable after sourcing shrc"

mkdir -p "$OUTPUT_DIR" || die "Cannot create $OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)"
STAGE="$OUTPUT_DIR/$RUN_ID"
[[ ! -e "$STAGE" ]] || die "Run directory exists: $STAGE (choose a fresh --run-id)"
mkdir -p "$STAGE/tests" || die "Cannot create staging directory"
ARCHIVE="$OUTPUT_DIR/spec_${RUN_ID}.tar.gz"
MANIFEST="$STAGE/summary.tsv"
printf 'benchmark\trun_rc\tcompare_rc\tstatus\trun_directory\n' > "$MANIFEST"

{
  printf 'run_id=%s\n' "$RUN_ID"
  printf 'spec_dir=%s\nlabel=%s\ndataset=%s\ntune=%s\ncopy_index=%s\n' \
    "$SPEC_DIR" "$LABEL" "$DATASET" "$TUNE" "$COPY_INDEX"
  printf 'started_utc=%s\n' "$(date -u +%FT%TZ)"
  printf 'kernel=%s\n' "$(uname -r)"
  printf 'benchmarks=%s\n' "${BENCHMARKS[*]}"
  printf 'm5=%s\n' "$USE_M5"
} > "$STAGE/metadata.txt"

ANY_FAILED=0
printf '\nSPEC experiment %s: %s\n' "$RUN_ID" "${BENCHMARKS[*]}"

for bench in "${BENCHMARKS[@]}"; do
  run_dir="$SPEC_DIR/benchspec/CPU/$bench/run/run_${TUNE}_${DATASET}_${LABEL}.${COPY_INDEX}"
  dest="$STAGE/tests/$bench"
  mkdir -p "$dest"
  printf '\n===== %s =====\n' "$bench"
  printf 'Run directory: %s\n' "$run_dir"

  if [[ ! -f "$run_dir/speccmds.cmd" ]]; then
    printf 'ERROR: No speccmds.cmd; runsetup is required\n' | tee "$dest/error.txt" >&2
    printf '%s\t-\t-\tMISSING_SETUP\t%s\n' "$bench" "$run_dir" >> "$MANIFEST"
    ANY_FAILED=1
    continue
  fi

  cp "$run_dir/speccmds.cmd" "$dest/"
  [[ ! -f "$run_dir/compare.cmd" ]] || cp "$run_dir/compare.cmd" "$dest/"
  # -nn preserves the environment captured by SPEC runsetup (crucial for
  # OMP_NUM_THREADS, LD_LIBRARY_PATH, and other benchmark-specific variables).
  (cd "$run_dir" && specinvoke -nn speccmds.cmd) > "$dest/dry-run.log" 2>&1
  dry_rc=$?
  if ((dry_rc != 0)); then
    printf 'ERROR: specinvoke -nn failed for %s\n' "$bench" >&2
    printf '%s\t%s\t-\tDRY_RUN_FAILED\t%s\n' "$bench" "$dry_rc" "$run_dir" >> "$MANIFEST"
    ANY_FAILED=1
    continue
  fi
  { printf 'set -e\n'; sed '/^specinvoke exit: rc=/d' "$dest/dry-run.log"; } > "$dest/run-commands.sh"

  # Capture generated/modified files without copying huge pre-staged input data.
  stamp="$dest/start.marker"
  touch "$stamp"
  printf '%s\n' "$(date -u +%FT%TZ)" > "$dest/start_utc.txt"

  if ((USE_M5)); then
    if ! m5 resetstats > "$dest/m5-reset.log" 2>&1; then
      printf 'WARNING: m5 resetstats failed for %s\n' "$bench" >&2
    fi
  fi

  (cd "$run_dir" && /bin/sh "$dest/run-commands.sh") > "$dest/run.log" 2>&1
  run_rc=$?

  if ((USE_M5)); then
    if ! m5 dumpstats > "$dest/m5-dump.log" 2>&1; then
      printf 'WARNING: m5 dumpstats failed for %s\n' "$bench" >&2
    fi
  fi

  compare_rc='NA'
  if [[ -f "$run_dir/compare.cmd" ]]; then
    (cd "$run_dir" && specinvoke compare.cmd) > "$dest/compare.log" 2>&1
    compare_rc=$?
  else
    printf 'compare.cmd missing\n' > "$dest/compare.log"
  fi

  # Only generated / modified files; preserve the run directory structure.
  mkdir -p "$dest/generated"
  files_list="$dest/generated-files.list0"
  (cd "$run_dir" && find . -type f -newer "$stamp" -printf '%P\0') > "$files_list"
  if [[ -s "$files_list" ]]; then
    if ! (cd "$run_dir" && tar --null -T "$files_list" -cf -) | \
      (cd "$dest/generated" && tar -xf -); then
      printf 'WARNING: could not collect all generated files for %s\n' "$bench" >&2
      ANY_FAILED=1
    fi
  fi
  rm -f "$files_list" "$stamp"
  printf '%s\n' "$(date -u +%FT%TZ)" > "$dest/end_utc.txt"

  if ((run_rc == 0)) && [[ "$compare_rc" == '0' ]]; then
    status='PASS'
  else
    status='FAIL'
    ANY_FAILED=1
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$bench" "$run_rc" "$compare_rc" "$status" "$run_dir" >> "$MANIFEST"
  printf '%s: run=%s compare=%s %s\n' "$bench" "$run_rc" "$compare_rc" "$status"
done

printf 'finished_utc=%s\n' "$(date -u +%FT%TZ)" >> "$STAGE/metadata.txt"
( cd "$OUTPUT_DIR" && tar -czf "$ARCHIVE" "$RUN_ID" ) || die "Archive failed"
(cd "$OUTPUT_DIR" && sha256sum "$(basename "$ARCHIVE")") > "$ARCHIVE.sha256"
printf '\nGuest archive: %s\n' "$ARCHIVE"
printf 'Summary:\n'
cat "$MANIFEST"

if ((USE_M5)); then
  host_name="$(basename "$ARCHIVE")"
  printf 'Exporting as %s to gem5 host output directory...\n' "$host_name"
  if m5 writefile "$ARCHIVE" "$host_name"; then
    printf 'Host export requested successfully: %s\n' "$host_name"
    # The sidecar hash is optional; export it as well for integrity checks.
    m5 writefile "$ARCHIVE.sha256" "$host_name.sha256" || {
      printf 'WARNING: checksum sidecar export failed\n' >&2
      ANY_FAILED=1
    }
  else
    printf 'ERROR: m5 writefile failed\n' >&2
    ANY_FAILED=1
  fi
fi

if ((EXIT_AT_END)); then
  if ((USE_M5)); then
    printf 'Requesting m5 exit...\n'
    m5 exit || ANY_FAILED=1
  else
    printf 'WARNING: --exit-at-end ignored because --no-m5 is active\n' >&2
  fi
fi
exit "$ANY_FAILED"
