# Parsec

https://dl.acm.org/doi/epdf/10.1145/1454115.1454128
parsec hakkında bir yayın.

## DOCKER alanı oluşturma
docker run \
  -u $UID:$GID \
  --volume .:/gem5 \
  --name gem5 \
  -dit \
  ghcr.io/gem5/ubuntu-24.04_all-dependencies:v25-1

## docker bağlanma
docker exec -it gem5 bash

## docker bağlanma (root)
docker exec -u root -it gem5 bash

## gem5 derleme
scons build/RISCV/gem5.opt

## gem5 yürütme
./build/RISCV/gem5.opt \ configs/example/gem5_library/riscv-ubuntu-run.py

## m5term kurulumu
https://www.gem5.org/documentation/general_docs/fullsystem/m5term
