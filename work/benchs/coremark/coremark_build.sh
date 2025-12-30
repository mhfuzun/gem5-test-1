make -C coremark \
  compile \
  PORT_DIR=posix \
  CC=riscv64-linux-gnu-gcc \
  XCFLAGS="-O2 -static -march=rv64gc -mabi=lp64d -DCORE_DEBUG=1"
