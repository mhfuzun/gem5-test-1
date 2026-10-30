# gem5 RISC-V FS SimPoint pipeline

This package preserves the boot checkpoint and produces a fresh benchmark-start checkpoint.

## 1. Host requirements

- A built `gem5` RISCV binary and the disk image used when the boot checkpoint was saved.
- SimPoint 3.2 binary (`simpoint`), GNU `gzip`, `bash`, Python 3.
- The RISC-V guest has `m5` installed.

Example SimPoint installation (host):

```bash
git clone https://github.com/Boulder-Computer-Architecture-Lab/SimPoint.git
cd SimPoint
make simpoint
# Binary: ./bin/simpoint
```

## 2. Place guest_launch_spec.sh on the disk image beforehand

The guest launcher should be available **in the restored guest snapshot**. For a checkpoint
already taken, files copied to the backing disk after that checkpoint may not behave as
expected because of disk-image copy-on-write and checkpoint state. If convenient, create
this launcher in the guest's running session with a here-document instead of relying on
new backing-disk edits.

The guest also needs an existing SPEC run directory and an actual run command.

## 3. Host launch

From the directory containing the Python and helper scripts:

```bash
export GEM5_BIN=/path/to/gem5/build/RISCV/gem5.opt
export CONFIG="$PWD/fs_simpoint.py"
export DISK_IMAGE=/path/to/ubuntu-riscv.raw.img
export BOOT_CPT=/path/to/boot_checkpoint
export SIMPOINT_BIN=/path/to/SimPoint/bin/simpoint
export OUT="$PWD/simpoint_results/gcc_01"
export INTERVAL=10000000
export MAX_K=10
export WARMUP=10000000
bash ./run_simpoint.sh
```

If `BOOT_CPT` is a folder containing `cpt.<tick>` directories rather than a direct
checkpoint, the Python configuration selects the first checkpoint, matching your
original code.

## 4. Guest action (once)

Connect to the guest gem5 console via its usual terminal method. **No extra guest file is required** if you directly enter the following sequence as a single line, replacing the working directory and real SPEC command:

```bash
cd /data/spec-runs/gcc
m5 exit; bash ./run_gcc.sh; m5 exit
```

The bash shell resumes after the first `m5 exit` once the benchmark-start checkpoint is restored, executes the benchmark, and exits gem5 again. Alternatively, if you want a reusable wrapper, execute:

```bash
bash /data/guest_launch_spec.sh /data/spec-runs/gcc bash ./run_gcc.sh
```

Replace the directory and command with your real SPEC run invocation.
The first `m5 exit` in this launcher takes the benchmark-start checkpoint and
terminates Stage A. The host wrapper then restores that new checkpoint, enables
BBV profiling and resumes the guest, which runs the SPEC command automatically.
After the benchmark finishes, the second `m5 exit` terminates Stage B; SimPoint
runs on the host.

Use separate output directories and benchmark-specific disk images/inputs where
necessary. This is one SPEC invocation per SimPoint clustering run.

## 5. Results

- `profile_gem5/simpoint.bb.gz`: gem5 BBV profile
- `analysis/simpoints.txt`: zero-based interval index, cluster ID
- `analysis/weights.txt`: weight, cluster ID
- `analysis/windows.tsv`: selected instruction ranges and proposed warmup start
- `analysis/metadata.json`: BBV intervals, wall times and metadata
- `prepare.log`, `profile.log`, `simpoint.log`: host logs
- `profile_gem5/stats.txt`: gem5 runtime statistics

**Important limitations**: BBV profiling is CPU-wide in FS mode: it includes
kernel, scheduler, and any other guest processes. SimPoint boundaries are based
on committed instructions after resuming the benchmark-start checkpoint, not
simulator ticks and not application-only instruction counts. SimPoint chooses
representative *intervals*, but by itself does not make checkpoints. The
windows file is the input for a subsequent Atomic-to-O3 checkpoint phase.

The host preparation/profile/cluster wall times are measured independently.
The reported Atomic profile instruction throughput is approximate because the
last incomplete BBV interval isn't counted. You cannot project O3 simulation
runtime from Atomic throughput without timing an O3 pilot run.

The package was syntax-checked only. It could not be exercised against your
particular gem5 build, installed SimPoint executable, checkpoint, or image.

`metadata.json` also reads `simSeconds`, `hostSeconds`, `simInsts`, and `simTicks` from gem5 `stats.txt` when present.
