# RHEL 7 Compatible gem5 Bundle

This directory contains a containerized build flow for producing a
self-contained gem5 bundle intended for `RHEL 7.9` era hosts.

## Why this exists

Your target machine reports:

- `RHEL 7.9`
- `glibc 2.17`
- `x86_64`
- `Python 3.8.18`

A gem5 binary built on a modern Ubuntu/Mint host links against a much newer
`glibc` and `libstdc++`, so it will not start on that system.

This flow builds gem5 inside a self-contained `Ubuntu 22.04` userspace and
packages the resulting binary together with:

- a private dynamic loader
- the required shared libraries
- a private Python runtime
- gem5 Python/config runtime files

The target machine then runs `run.sh`, which uses the bundled loader and libs
instead of the host's system copies.

## What gets produced

Running `build_bundle.sh` creates an output directory like:

```text
bundle-out/RISCV/
  bin/gem5.opt
  lib/
  python/
  gem5/
  run.sh
  BUILD_INFO.txt
  BINARY_LDD.txt
  gem5-RISCV-rhel7-compat-bundle.zip
```

## Build

From the gem5 repo root:

```bash
bash util/dockerfiles/rhel7-compat-bundle/build_bundle.sh
```

Optional environment overrides:

```bash
ISA=RISCV JOBS=16 IMAGE_TAG=my-gem5-bundle OUT_DIR=$PWD/my-bundle \
  bash util/dockerfiles/rhel7-compat-bundle/build_bundle.sh
```

## Run on the target host

Extract the zip on the target machine and run:

```bash
./run.sh configs/example/se.py ...
```

If you use your own config scripts, place them next to the bundle or pass an
absolute path when invoking `run.sh`.

## Important note

This is a pragmatic compatibility bundle, not a fully static executable.
It works by carrying a private userspace with gem5. That is much more realistic
for gem5 than trying to force a single fully static binary.
