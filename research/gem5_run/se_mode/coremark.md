### derlemek için
```
make PORT_DIR=linux \
    CC=riscv64-linux-gnu-gcc \
    NO_LIBRT=1 \
    ITERATIONS=1000 \
    XCFLAGS="--sysroot=/opt/riscv/sysroot \
              -march=rv64gc \
              -mabi=lp64d \
              -O3 \
              -static" \
    compile
```
