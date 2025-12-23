dir=./coremark
make -C $dir \
     PORT_DIR=posix clean
make -C $dir \
     CC=riscv64-linux-gnu-gcc \
     PORT_DIR=posix \
     ITERATIONS=10 \
     XCFLAGS="-static -O3 -march=rv64g -mabi=lp64d" \
     link