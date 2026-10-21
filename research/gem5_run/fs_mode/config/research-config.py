import argparse
import shlex
from pathlib import Path

import m5
from m5.objects import SrcClockDomain

from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import (
    PrivateL1SharedL2CacheHierarchy,
)
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    BootloaderResource,
    DiskImageResource,
    KernelResource,
    obtain_resource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator

parser = argparse.ArgumentParser()

parser.add_argument(
    "--kernel",
    default="riscv-linux-5.15.180",
)
parser.add_argument(
    "--bootloader",
    default="riscv-bootloader-opensbi-1.3.1",
)
parser.add_argument("--disk-image", required=True)
parser.add_argument(
    "--command-line",
    default="console=ttyS0 root=/dev/vda1 ro init=/sbin/init",
)
parser.add_argument(
    "--checkpoint-dir",
    default="m5out_rasCheckpoint",
)

parser.add_argument("--cpu-clock", default="2GHz")
parser.add_argument("--sys-clock", default="1GHz")
parser.add_argument("--mem-size", default="20GiB")
parser.add_argument("--l1i_size", default="64KiB")
parser.add_argument("--l1d_size", default="64KiB")
parser.add_argument("--l2_size", default="2MiB")

args = parser.parse_args()


def resolve_resource(spec, resource_class):
    path = Path(spec).expanduser()

    if path.is_file():
        return resource_class(local_path=str(path.resolve()))

    if "/" in spec or spec.startswith("."):
        parser.error(f"Resource file not found: {spec}")

    return obtain_resource(resource_id=spec)


kernel = resolve_resource(args.kernel, KernelResource)
bootloader = resolve_resource(args.bootloader, BootloaderResource)

disk_image = DiskImageResource(
    local_path=str(Path(args.disk_image).expanduser().resolve()),
    root_partition="1",
)

print(f"Kernel: {kernel.get_local_path()}")
print(f"Bootloader: {bootloader.get_local_path()}")
print(f"Disk: {disk_image.get_local_path()}")


processor = SimpleProcessor(
    cpu_type=CPUTypes.ATOMIC,
    isa=ISA.RISCV,
    num_cores=1,
)

memory = DualChannelDDR4_2400(size=args.mem_size)

cache = PrivateL1SharedL2CacheHierarchy(
    l1i_size=args.l1i_size,
    l1d_size=args.l1d_size,
    l2_size=args.l2_size,
)

board = RiscvBoard(
    clk_freq=args.sys_clock,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache,
)

# Separate CPU clock domain.
cpu_clk = SrcClockDomain(
    clock=args.cpu_clock,
    voltage_domain=board.clk_domain.voltage_domain,
)

for core in processor.get_cores():
    core.get_simobject().clk_domain = cpu_clk


board.set_kernel_disk_workload(
    kernel=kernel,
    bootloader=bootloader,
    disk_image=disk_image,
    kernel_args=shlex.split(args.command_line),
)


checkpoint_dir = Path(args.checkpoint_dir).resolve()


def exit_handler():
    checkpoint_dir.parent.mkdir(
        parents=True,
        exist_ok=True,
    )
    m5.checkpoint(str(checkpoint_dir))
    print(f"Checkpoint saved: {checkpoint_dir}")
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: exit_handler(),
    },
)

simulator.run()
