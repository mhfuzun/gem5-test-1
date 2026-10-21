#!/usr/bin/env bash
# Guest: run ONE prepared SPEC benchmark and transfer outputs to gem5 host.
# Requires only bash, GNU find/awk/tar/gzip and m5 (unless --no-m5).
set -euo pipefail

usage() {
  cat <<'HELP'
Usage: 02_run_guest.sh --run-dir PATH --out-dir PATH --test BENCHMARK [--id ID] [--no-m5]
Example:
  ./02_run_guest.sh --run-dir /data/spec2017/519.lbm_r/run_base_refrate_mytest.0000 \
      --out-dir /data/results --test 519.lbm_r --id run01

Run directory is modified only in this guest's COW disk layer. The archive
contains logs, metadata and newly-created/changed output files, NOT the inputs.
With m5: m5 resetstats -> benchmark -> m5 dumpstats -> m5 writefile.
HELP
}

RUN_DIR=""; OUT_DIR=""; TEST=""; RUN_ID="run01"; NO_M5=0
while (( $# )); do
  case "$1" in
    --run-dir) RUN_DIR=${2:?}; shift 2 ;;
    --out-dir) OUT_DIR=${2:?}; shift 2 ;;
    --test) TEST=${2:?}; shift 2 ;;
    --id) RUN_ID=${2:?}; shift 2 ;;
    --no-m5) NO_M5=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$RUN_DIR" && -n "$OUT_DIR" && -n "$TEST" ]] || { usage >&2; exit 2; }
[[ "$TEST" =~ ^[0-9]{3}\.[A-Za-z0-9_]+$ ]] || { echo 'Invalid --test' >&2; exit 2; }
[[ "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || { echo 'Invalid --id' >&2; exit 2; }
RUN_DIR=$(realpath "$RUN_DIR")
[[ -f "$RUN_DIR/run_benchmark.sh" && -f "$RUN_DIR/compare.cmd" ]] || {
  echo "Missing run_benchmark.sh / compare.cmd in $RUN_DIR" >&2; exit 1;
}
[[ $(basename "$(dirname "$RUN_DIR")") == "$TEST" ]] || {
  echo "Expected run directory below .../$TEST/; got $RUN_DIR" >&2; exit 1;
}
if (( !NO_M5 )); then
  command -v m5 >/dev/null || { echo 'm5 utility missing' >&2; exit 1; }
fi
mkdir -p "$OUT_DIR"
OUT_DIR=$(realpath "$OUT_DIR")
[[ "$OUT_DIR/" != "$RUN_DIR/"* ]] || { echo 'Output directory must be outside run directory' >&2; exit 1; }
ARCHIVE="$OUT_DIR/spec_${TEST}_${RUN_ID}.tar.gz"
STAGE=$(mktemp -d "$OUT_DIR/.${TEST}.${RUN_ID}.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
mkdir -p "$STAGE/payload/run_delta" "$STAGE/payload/logs"

# Metadata snapshots detect both new files and modified preexisting files,
# without relying on host/guest wall-clock timestamps being synchronized.
snapshot() {
  ( cd "$RUN_DIR" && \
    find . \( -type f -o -type l \) \
      -printf '%P\t%s\t%T@\t%C@\t%y\n' | LC_ALL=C sort )
}
snapshot > "$STAGE/before.tsv"
START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
run_rc=0
m5_rc=0
if (( !NO_M5 )); then
  if ! m5 resetstats; then
    echo 'm5 resetstats failed; not starting benchmark' >&2
    exit 1
  fi
fi

# Benchmark stdout/stderr redirections are embedded in run_benchmark.sh by SPEC.
# This log collects shell messages and errors not already redirected by SPEC.
bash "$RUN_DIR/run_benchmark.sh" > "$STAGE/payload/logs/runner.log" 2>&1 || run_rc=$?

if (( !NO_M5 )); then
  m5 dumpstats || m5_rc=$?
fi
END=$(date -u +%Y-%m-%dT%H:%M:%SZ)
snapshot > "$STAGE/after.tsv"

# Input set is unchanged in the prepared host tree; capture every modified/new file.
# SPEC benchmark file names are assumed not to contain tabs/newlines.
awk -F '\t' '
  NR==FNR { old[$1]=$0; next }
  !($1 in old) || old[$1]!=$0 { print $1 }
' "$STAGE/before.tsv" "$STAGE/after.tsv" > "$STAGE/changed.txt"
awk -F '\t' '
  NR==FNR { now[$1]=1; next }
  !($1 in now) { print $1 }
' "$STAGE/after.tsv" "$STAGE/before.tsv" > "$STAGE/payload/deleted.txt"

if [[ -s "$STAGE/changed.txt" ]]; then
  tr '\n' '\0' < "$STAGE/changed.txt" > "$STAGE/changed.null"
  tar -C "$RUN_DIR" --null --verbatim-files-from \
    -T "$STAGE/changed.null" -cf - | \
    tar -C "$STAGE/payload/run_delta" -xf -
fi
cp "$STAGE/changed.txt" "$STAGE/payload/changed.txt"
cat > "$STAGE/payload/metadata.txt" <<META
format=spec-gem5-v1
test=$TEST
run_dir=$(basename "$RUN_DIR")
run_id=$RUN_ID
exit_code=$run_rc
m5_dump_status=$m5_rc
started_utc=$START
finished_utc=$END
META

# Archive relative payload layout, so the host can validate without guest tools.
tar -C "$STAGE/payload" -cf - . | gzip -1 > "$ARCHIVE"
( cd "$OUT_DIR" && sha256sum "$(basename "$ARCHIVE")" > "$(basename "$ARCHIVE").sha256" )
printf 'Benchmark exit code: %s; m5 dump status: %s\n' "$run_rc" "$m5_rc"
printf 'Collected %s changed files into %s\n' "$(wc -l < "$STAGE/changed.txt")" "$ARCHIVE"
if (( !NO_M5 )); then
  # Guest pathname is local; basename is the requested host filename.
  m5 writefile "$ARCHIVE" "$(basename "$ARCHIVE")"
  m5 writefile "$ARCHIVE.sha256" "$(basename "$ARCHIVE.sha256")"
fi
if (( run_rc != 0 || m5_rc != 0 )); then
  echo 'WARNING: benchmark or stats operation failed; partial results exported.' >&2
  exit 1
fi
