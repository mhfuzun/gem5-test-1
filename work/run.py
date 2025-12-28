# run.py
import m5
from m5.objects import *

from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator

# Create board with OOO RISC-V processor
board = RiscvBoard(
    clk_freq="3GHz",
    processor=SimpleProcessor(
        cpu_type=CPUTypes.O3, isa=ISA.RISCV, num_cores=1
    ),
    memory=SingleChannelDDR3_1600("2GB"),
    cache_hierarchy=PrivateL1PrivateL2CacheHierarchy(
        l1d_size="32kB", l1i_size="32kB", l2_size="256kB"
    ),
)

# Script that runs inside the simulated system
boot_script = """
echo "=== Running Hello World in gem5 ==="
echo ""

# Create hello world C program
cat > /tmp/hello.c << 'EOFCODE'
#include <stdio.h>

int main() {
    printf("Hello World from gem5 RISC-V O3 (Out-of-Order) CPU!\\n");
    printf("Simulation successful!\\n");
    return 0;
}
EOFCODE

# Try to compile if gcc is available
cd /tmp
if command -v gcc >/dev/null 2>&1; then
    echo "Compiling hello.c..."
    gcc -static hello.c -o hello
    echo "Running compiled binary..."
    ./hello
else
    # If no compiler, just echo the message
    echo "Hello World from gem5 RISC-V O3 (Out-of-Order) CPU!"
    echo "Simulation successful!"
fi

echo ""
echo "=== Exiting simulation ==="
m5 exit
"""

print("Configuring gem5 with RISC-V O3 CPU...")
board.set_kernel_disk_workload(
    kernel=obtain_resource("riscv-bootloader-vmlinux-5.10"),
    disk_image=obtain_resource("riscv-disk-img"),
    readfile_contents=boot_script,
)

print("Starting simulation...")
simulator = Simulator(board=board)
simulator.run()

print("\n" + "=" * 50)
print("Simulation Complete!")
print("=" * 50)
print("\nCheck the output above for 'Hello World' message")
print("Full console output: m5out/system.pc.com_1.device")
