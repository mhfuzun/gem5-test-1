riscv64-linux-gnu-gcc \
    -O2 \
    -march=rv64g \
    -mabi=lp64d \
    main.c \
    -o hello
file hello
