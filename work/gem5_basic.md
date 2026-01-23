# Gem5 Basic

## SE modu
$GEM5PATH/build/RISCV/gem5.opt \
  $GEM5PATH/configs/deprecated/example/se.py \
  --cmd=$GEM5PATH/work/benchs/hello_world/hello \
  --cpu-type=O3CPU \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=1GB

## Konata Kullanımı
```bash
# gem5.opt [gem5 options] script.py [script options]
$GEM5PATH/build/RISCV/gem5.opt \
  --debug-flags=O3PipeView,O3CPUAll \
  --debug-start=0 \
  --debug-file=trace.out \
  $GEM5PATH/configs/deprecated/example/se.py \
  --cmd=$GEM5PATH/work/benchs/hello_world/hello \
  --cpu-type=O3CPU \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=1GB
```

## Debug işleri
```bash
$GEM5PATH/build/RISCV/gem5.debug \
  $GEM5PATH/configs/deprecated/example/se.py \
  --cmd=$GEM5PATH/test/konata_test/hello_world/basic \
  --cpu-type=O3CPU \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=1GB
```
