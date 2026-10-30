# SPEC reference instruction counts

[count_ref_instructions.py](./count_ref_instructions.py) discovers every
`5xx.*_r/run*/speccmds.cmd` under
[`research-out/spec-runs/ref`](../../research-out/spec-runs/ref). It runs all
33 prepared commands from the 22 rate benchmarks with eight parallel QEMU
processes by default. Each process is pinned to a different physical core
when eight physical cores are available. OpenMP is limited to one thread.

```bash
python3 research/spec-ref-inst-count/count_ref_instructions.py --list
python3 research/spec-ref-inst-count/count_ref_instructions.py --smoke-only
python3 research/spec-ref-inst-count/count_ref_instructions.py --jobs 8 \
    --resume --background
python3 research/spec-ref-inst-count/count_ref_instructions.py --status
```

The default QEMU and plugin paths are the supplied `qemu-count/build` paths
under the user's home directory. Override them with `--qemu`, `--plugin`, or
`--sysroot`. Linux `bwrap` and `taskset` are required. These commands need
access to QEMU outside the repository sandbox.

Before any reference commands, the smoke test compiles a tiny C file using
the prepared RISC-V `cpugcc`. It checks the exit status, assembly output, and
a positive instruction count. Its artifacts are in `inst-counts/_smoke/000`.
The smoke test uses a separate file, so it does not shorten a reference run.

## Outputs and source protection

Outputs are written beneath `research-out/spec-runs/ref`:

```text
inst-counts.txt                       # Updated while running, final totals
inst-counts/
  runner.log                         # Starts, completions, errors
  runner.pid                         # Controller PID
  runner.lock                        # Prevents simultaneous controllers
  configuration.json                 # CPU affinity, tool paths and hashes
  source-before.json                 # Original file hashes and metadata
  source-after-smoke.json             # Created by --smoke-only
  source-after.json                   # End-of-batch integrity snapshot
  summary.json                       # End-of-batch status
  502.gcc_r/
    000/
      command.txt                    # Actual isolated QEMU invocation
      result.json                    # Status, exact count, duration, PID
      qemu.plugin.log                # Original QEMU instruction counter
      <SPEC stdout/err filenames>
      work/                          # Independent copy and generated files
      tmp/                           # Private guest /tmp
    001/
    ...
```

Input trees are copied without hardlinks. Bubblewrap mounts the host
filesystem read-only and makes only the individual command's output folder
writable. Each command has its own working directory and `/tmp`. This also
protects files that a benchmark opens internally, rather than through shell
redirection. Source content hashes, modes, and modification times must match
before and after the batch. The original run wrappers are never executed.

The report has one row per subcommand and a separate total per benchmark.
Counts are parsed as Python integers without floating-point conversion.
They include all guest userspace instructions, including libraries and the
dynamic loader; they exclude kernel instructions. This runs the prepared
workloads without performing SPEC output comparisons or producing SPEC scores.

Nonzero exits, missing counters, and malformed counters are marked failed;
their partial counts are excluded from successful totals. Other commands
continue, including other commands of the same benchmark. Incomplete benchmark
totals are explicitly marked. No default timeout is imposed on full ref
workloads; `--timeout SECONDS` optionally sets one per command.

`--background` detaches the controller and redirects its output to
`inst-counts/runner.log`. The report refreshes at least every 15 seconds. The
run continues after the terminal or chat tool invocation ends. Send SIGTERM
to the PID in `runner.pid` for a graceful stop. `--resume` retains both
successful and failed commands and reruns unfinished commands, archiving
interrupted directories beneath `inst-counts/_interrupted`. Tool hashes,
source fingerprints, and CPU settings must match the original batch.

## Local checks

```bash
python3 -m unittest discover -s research/spec-ref-inst-count -v
python3 -m py_compile research/spec-ref-inst-count/*.py
```
