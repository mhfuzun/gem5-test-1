make -C coremark \
  clean compile \
  PORT_DIR=posix \
  CC=riscv64-linux-gnu-gcc \
  ITERATIONS=1 \
  ADDITIONAL_SRCS=$(realpath ../../../util/m5/src/abi/riscv/m5op.S) \
  XCFLAGS="-I$(realpath ../../../include) -O2 -static -march=rv64gc -mabi=lp64d -DCORE_DEBUG=0 -DM5OPS"
