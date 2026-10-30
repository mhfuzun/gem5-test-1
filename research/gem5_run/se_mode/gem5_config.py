#!/usr/bin/env python3
# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""gem5-side configuration used by the three host orchestration scripts.

This file is not intended to be invoked directly. The host scripts create a
fully resolved runtime JSON document and launch this file with gem5.
"""

import argparse
import importlib.util
import inspect
import json
import shutil
import sys
from pathlib import Path

import m5
import m5.objects as objects
from m5 import stats as m5_stats
from m5.objects import RedirectPath

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic import (
    private_l1_private_l2_walk_cache_hierarchy as private_cache,
)
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    BinaryResource,
    FileResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.RISCV)


def read_config(path):
    with Path(path).open(encoding="utf-8") as source:
        return json.load(source)


def write_status(config, status):
    path = Path(config["runtime_status_path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(status, indent=2, sort_keys=True) + "\n")


def stage_path(value, workdir):
    if value is None:
        return None
    path = Path(value)
    return path if path.is_absolute() else workdir / path


def set_nested_parameter(target, name, value):
    fields = name.split(".")
    owner = target
    for field in fields[:-1]:
        if not hasattr(owner, field):
            raise ValueError(f"unknown parameter path {name!r}")
        owner = getattr(owner, field)
    if not hasattr(owner, fields[-1]):
        raise ValueError(f"unknown parameter {name!r}")
    setattr(owner, fields[-1], value)


def load_configuration_module(path, board, processor, cache_hierarchy, config):
    if not path:
        return
    module_path = Path(path).resolve()
    spec = importlib.util.spec_from_file_location(
        "se_mode_user_configuration", module_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load configuration module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not hasattr(module, "configure"):
        raise RuntimeError(
            f"configuration module must define configure(...): {module_path}"
        )
    module.configure(
        board=board,
        processor=processor,
        cache_hierarchy=cache_hierarchy,
        config=config,
    )


def create_board(config, cpu_type, checkpoint=None):
    workload = config["workload"]
    workdir = Path(config["stage_workdir"]).resolve()
    system = config["system"]

    processor_arguments = {
        "cpu_type": cpu_type,
        "isa": ISA.RISCV,
        "num_cores": 1,
    }
    if "clk_freq" in inspect.signature(SimpleProcessor).parameters:
        processor_arguments["clk_freq"] = system["clock"]
    processor = SimpleProcessor(**processor_arguments)
    if cpu_type == CPUTypes.O3:
        cache_config = config["o3"]["cache"]
        cache_hierarchy = private_cache.PrivateL1PrivateL2WalkCacheHierarchy(
            l1i_size=cache_config["l1i_size"],
            l1d_size=cache_config["l1d_size"],
            l2_size=cache_config["l2_size"],
        )
        core = processor.get_cores()[0].core
        branch_type = config["o3"].get("branch_predictor")
        if branch_type:
            predictor_class = getattr(objects, branch_type, None)
            if predictor_class is None:
                raise ValueError(
                    f"unknown branch predictor class: {branch_type}"
                )
            core.branchPred = predictor_class()
        predictor_params = config["o3"].get("branch_predictor_params", {})
        for name, value in predictor_params.items():
            set_nested_parameter(core.branchPred, name, value)
        for name, value in config["o3"].get("cpu_params", {}).items():
            set_nested_parameter(core, name, value)
    else:
        cache_hierarchy = NoCache()

    memory = SingleChannelDDR3_1600(size=system["memory_size"])
    board = SimpleBoard(
        clk_freq=system["clock"],
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
    )

    if workload.get("interpreter_root"):
        from m5.core import setInterpDir

        setInterpDir(workload["interpreter_root"])

    binary = Path(workload["binary"])
    host_binary = binary if binary.is_absolute() else workdir / binary
    stdin_path = stage_path(workload.get("stdin"), workdir)
    stdout_path = stage_path(workload.get("stdout"), workdir)
    stderr_path = stage_path(workload.get("stderr"), workdir)
    for output in (stdout_path, stderr_path):
        if output is not None:
            output.parent.mkdir(parents=True, exist_ok=True)

    board.set_se_binary_workload(
        binary=BinaryResource(local_path=str(host_binary)),
        arguments=workload.get("arguments", []),
        stdin_file=(
            FileResource(local_path=str(stdin_path))
            if stdin_path is not None
            else None
        ),
        stdout_file=stdout_path,
        stderr_file=stderr_path,
        env_list=[
            f"{key}={value}"
            for key, value in sorted(workload.get("environment", {}).items())
        ],
        checkpoint=Path(checkpoint).resolve() if checkpoint else None,
    )

    process = processor.get_cores()[0].core.workload[0]
    process.cwd = workload["guest_working_directory"]
    process.cmd = [workload["guest_binary"]] + workload.get("arguments", [])

    redirects = [
        RedirectPath(
            app_path=workload["guest_working_directory"],
            host_paths=[str(workdir)],
        )
    ]
    redirects.extend(
        RedirectPath(app_path=item["guest"], host_paths=[item["host"]])
        for item in workload.get("path_redirects", [])
    )
    board.redirect_paths = redirects

    if cpu_type == CPUTypes.O3:
        load_configuration_module(
            config["o3"].get("configuration_module"),
            board,
            processor,
            cache_hierarchy,
            config,
        )
    return board, processor


def run_profile(config):
    board, processor = create_board(config, CPUTypes.ATOMIC)
    processor.get_cores()[0].core.addSimPointProbe(config["interval"])
    simulator = Simulator(board=board)
    if config.get("max_insts", 0):
        simulator.schedule_max_insts(config["max_insts"])
    simulator.run()
    cause = simulator.get_last_exit_event_cause()
    normal_exit = cause == "exiting with last active thread context"
    status = {
        "mode": "profile",
        "complete": normal_exit,
        "program_completed": normal_exit,
        "exit_cause": cause,
        "exit_code": simulator.get_last_exit_event_code(),
        "committed_instructions": processor.get_total_instructions(),
        "tick": simulator.get_current_tick(),
    }
    write_status(config, status)
    return 0 if normal_exit or config.get("allow_partial_profile") else 2


def run_checkpoints(config):
    board, processor = create_board(config, CPUTypes.ATOMIC)
    points = sorted(
        config["points"],
        key=lambda point: (
            point["checkpoint_instruction"],
            point["checkpoint_id"],
        ),
    )
    grouped = []
    for point in points:
        if not grouped or grouped[-1][0] != point["checkpoint_instruction"]:
            grouped.append((point["checkpoint_instruction"], []))
        grouped[-1][1].append(point)

    status = {
        "mode": "checkpoint",
        "complete": False,
        "created": [],
    }
    group_index = 0
    simulator = None

    def checkpoint_handler():
        nonlocal group_index
        while True:
            expected_instruction, group_points = grouped[group_index]
            observed_instruction = processor.get_total_instructions()
            for point in group_points:
                checkpoint_path = (
                    Path(config["checkpoint_root"]) / point["checkpoint_id"]
                )
                simulator.save_checkpoint(checkpoint_path)
                snapshot_path = Path(point["filesystem_snapshot"])
                shutil.copytree(
                    config["stage_workdir"], snapshot_path, symlinks=True
                )
                status["created"].append(
                    {
                        "checkpoint_id": point["checkpoint_id"],
                        "expected_instruction": expected_instruction,
                        "observed_instruction": observed_instruction,
                        "path": str(checkpoint_path),
                        "filesystem_snapshot": str(snapshot_path),
                    }
                )
            group_index += 1
            finished = group_index == len(grouped)
            status["complete"] = finished
            write_status(config, status)
            yield finished

    simulator = Simulator(
        board=board,
        on_exit_event={ExitEvent.SIMPOINT_BEGIN: checkpoint_handler()},
    )
    # Simulator.schedule_simpoint() in gem5 25.1 calls a non-existent public
    # wrapper. Workload setup uses this supported core hook as well.
    processor.get_cores()[0]._set_simpoint(
        [item[0] for item in grouped], board_initialized=False
    )
    simulator.run()
    status.update(
        {
            "exit_cause": simulator.get_last_exit_event_cause(),
            "exit_code": simulator.get_last_exit_event_code(),
            "committed_instructions": processor.get_total_instructions(),
            "tick": simulator.get_current_tick(),
        }
    )
    status["complete"] = len(status["created"]) == len(points)
    write_status(config, status)
    return 0 if status["complete"] else 2


def run_o3(config):
    board, processor = create_board(
        config, CPUTypes.O3, checkpoint=config["checkpoint_path"]
    )
    warmup = config["warmup_instructions"]
    measurement = config["measurement_instructions"]
    phase = "warmup" if warmup else "measurement"
    simulation_start_total = None
    warmup_end_total = None
    status = {
        "mode": "o3",
        "complete": False,
        "phase": phase,
        "requested_warmup_instructions": warmup,
        "requested_measurement_instructions": measurement,
    }
    simulator = None

    def max_inst_handler():
        nonlocal phase, warmup_end_total
        while True:
            current_total = processor.get_total_instructions()
            if phase == "warmup":
                warmup_end_total = current_total
                status["warmup_end_total_instructions"] = current_total
                status["observed_warmup_instructions"] = (
                    current_total - simulation_start_total
                )
                status["warmup_overshoot_instructions"] = (
                    status["observed_warmup_instructions"] - warmup
                )
                status["phase"] = "measurement"
                m5_stats.reset()
                simulator.schedule_max_insts(measurement)
                phase = "measurement"
                write_status(config, status)
                yield False
            else:
                status["measurement_end_total_instructions"] = current_total
                measurement_start = (
                    warmup_end_total
                    if warmup_end_total is not None
                    else simulation_start_total
                )
                actual_measurement = current_total - measurement_start
                status["observed_measurement_instructions"] = (
                    actual_measurement
                )
                status["measurement_total_instruction_delta"] = (
                    actual_measurement
                )
                status["measurement_overshoot_instructions"] = (
                    actual_measurement - measurement
                )
                status["measurement_exact"] = actual_measurement == measurement
                status["phase"] = "complete"
                status["complete"] = actual_measurement >= measurement
                m5_stats.dump()
                write_status(config, status)
                yield True

    simulator = Simulator(
        board=board,
        on_exit_event={ExitEvent.MAX_INSTS: max_inst_handler()},
    )
    # Instantiate explicitly so the restored committed-instruction counter is
    # available as a baseline. Wide O3 commit may retire a few instructions in
    # the cycle that raises MAX_INSTS; the actual delta is recorded above.
    simulator._instantiate()
    simulation_start_total = processor.get_total_instructions()
    status["simulation_start_total_instructions"] = simulation_start_total
    if not warmup:
        m5_stats.reset()
    simulator.schedule_max_insts(warmup if warmup else measurement)
    simulator.run()
    status.update(
        {
            "exit_cause": simulator.get_last_exit_event_cause(),
            "exit_code": simulator.get_last_exit_event_code(),
            "committed_instructions": processor.get_total_instructions(),
            "tick": simulator.get_current_tick(),
        }
    )
    write_status(config, status)
    return 0 if status["complete"] else 2


def run_o3_full(config):
    """Run O3 from program entry to successful SE process termination."""
    board, processor = create_board(config, CPUTypes.O3)
    simulator = Simulator(board=board)
    if config.get("max_insts"):
        simulator.schedule_max_insts(config["max_insts"])
    simulator.run()
    cause = simulator.get_last_exit_event_cause()
    code = simulator.get_last_exit_event_code()
    complete = cause == "exiting with last active thread context" and code == 0
    m5_stats.dump()
    write_status(
        config,
        {
            "mode": "o3_full",
            "complete": complete,
            "program_completed": complete,
            "exit_cause": cause,
            "exit_code": code,
            "committed_instructions": processor.get_total_instructions(),
            "tick": simulator.get_current_tick(),
        },
    )
    return 0 if complete else 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode", choices=("profile", "checkpoint", "o3", "o3_full")
    )
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    config = read_config(args.config)
    if args.mode == "profile":
        return run_profile(config)
    if args.mode == "checkpoint":
        return run_checkpoints(config)
    if args.mode == "o3_full":
        return run_o3_full(config)
    return run_o3(config)


if __name__ == "__m5_main__":
    sys.exit(main())
