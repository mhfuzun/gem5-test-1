make -C coremark \
  clean compile \
  PORT_DIR=posix \
  CC=riscv64-linux-gnu-gcc \
  ITERATIONS=500 \
  XCFLAGS="-O2 -static -march=rv64gc -mabi=lp64d -DCORE_DEBUG=0"
