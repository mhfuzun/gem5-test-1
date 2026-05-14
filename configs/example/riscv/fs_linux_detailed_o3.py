# Copyright (c) 2026 OpenAI-Codex
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2026
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
Detailed RISC-V full-system configuration focused on RiscvO3CPU.

This script keeps the standard Linux FS flow, but exposes a much larger set of
knobs for:
  - O3 CPU widths, queues, registers and pipeline delays
  - L1I/L1D/L2/walker cache sizes, latencies and MSHRs
  - memory channels and DRAM type through gem5's common memory options
  - branch predictor, BTB, RAS and indirect predictor defaults

If you add a custom branch predictor SimObject under src/cpu/pred, set
DEFAULT_BP_TYPE below to your new type name. The script also keeps gem5's
generic --param option, so nested/custom predictor fields can still be tuned
without editing this file again, e.g.:
  --param 'system.cpu[0].branchPred.someCustomField = 42'
"""

import argparse
from os import path

import m5
from m5.objects import *
from m5.util import (
    addToPath,
    fatal,
    warn,
)
from m5.util.fdthelper import *

from gem5.isas import ISA
from gem5.utils.requires import requires

addToPath("../../")

from common import (
    CpuConfig,
    MemConfig,
    ObjectList,
    Options,
    Simulation,
)
from common.Benchmarks import *
from common.CacheConfig import ExternalCacheFactory
from common.Caches import IOCache
from common.FSConfig import *
from common.SysPaths import *

requires(isa_required=ISA.RISCV)

DEFAULT_O3_CPU_TYPE = "RiscvO3CPU"
# Change this default to your custom predictor's SimObject type after adding it.
DEFAULT_BP_TYPE = "TournamentBP"
DEFAULT_INDIRECT_BP_TYPE = "SimpleIndirectPredictor"


def cpu_is_o3(cpu_cls):
    return cpu_cls is not None and issubclass(cpu_cls, BaseO3CPU)


def configure_cpu_instance(core, args):
    if isinstance(core, BaseO3CPU):
        configure_cpu_o3(core, args)
        configure_branch_predictor(core, args)


def generateMemNode(state, mem_range):
    node = FdtNode(f"memory@{int(mem_range.start):x}")
    node.append(FdtPropertyStrings("device_type", ["memory"]))
    node.append(
        FdtPropertyWords(
            "reg",
            state.addrCells(mem_range.start)
            + state.sizeCells(mem_range.size()),
        )
    )
    return node


def generateDtb(system):
    state = FdtState(addr_cells=2, size_cells=2, cpu_cells=1)
    root = FdtNode("/")
    root.append(state.addrCellsProperty())
    root.append(state.sizeCellsProperty())
    root.appendCompatible(["riscv-virtio"])

    for mem_range in system.mem_ranges:
        root.append(generateMemNode(state, mem_range))

    sections = [*system.cpu, system.platform]

    for section in sections:
        for node in section.generateDeviceTree(state):
            if node.get_name() == root.get_name():
                root.merge(node)
            else:
                root.append(node)

    node = FdtNode("chosen")
    node.append(FdtPropertyStrings("bootargs", [system.workload.command_line]))
    node.append(FdtPropertyStrings("stdout-path", ["/uart@10000000"]))
    root.append(node)

    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtsFile(path.join(m5.options.outdir, "device.dts"))
    fdt.writeDtbFile(path.join(m5.options.outdir, "device.dtb"))


def get_prefetcher(prefetcher_type):
    if prefetcher_type is None:
        return NULL

    prefetcher_class = ObjectList.hwp_list.get(prefetcher_type)
    return prefetcher_class()


def get_replacement_policy(policy_type):
    if policy_type is None:
        return None

    policy_class = ObjectList.rp_list.get(policy_type)
    return policy_class()


def make_cache(
    *,
    size,
    assoc,
    tag_latency,
    data_latency,
    response_latency,
    mshrs,
    tgts_per_mshr,
    write_buffers=None,
    prefetcher=None,
    replacement_policy=None,
    is_read_only=False,
    writeback_clean=False,
    **extra_args,
):
    cache_args = {
        "size": size,
        "assoc": assoc,
        "tag_latency": tag_latency,
        "data_latency": data_latency,
        "response_latency": response_latency,
        "mshrs": mshrs,
        "tgts_per_mshr": tgts_per_mshr,
        "is_read_only": is_read_only,
        "writeback_clean": writeback_clean,
    }

    if write_buffers is not None:
        cache_args["write_buffers"] = write_buffers

    if prefetcher not in (None, NULL):
        cache_args["prefetcher"] = prefetcher

    if replacement_policy is not None:
        cache_args["replacement_policy"] = replacement_policy

    cache_args.update(extra_args)

    return Cache(**cache_args)


def build_l1i_cache(args):
    return make_cache(
        size=args.l1i_size,
        assoc=args.l1i_assoc,
        tag_latency=args.l1i_tag_latency,
        data_latency=args.l1i_data_latency,
        response_latency=args.l1i_response_latency,
        mshrs=args.l1i_mshrs,
        tgts_per_mshr=args.l1i_tgts_per_mshr,
        prefetcher=get_prefetcher(args.l1i_hwp_type),
        replacement_policy=get_replacement_policy(args.l1i_rp_type),
        is_read_only=True,
        writeback_clean=True,
    )


def build_l1d_cache(args):
    return make_cache(
        size=args.l1d_size,
        assoc=args.l1d_assoc,
        tag_latency=args.l1d_tag_latency,
        data_latency=args.l1d_data_latency,
        response_latency=args.l1d_response_latency,
        mshrs=args.l1d_mshrs,
        tgts_per_mshr=args.l1d_tgts_per_mshr,
        write_buffers=args.l1d_write_buffers,
        prefetcher=get_prefetcher(args.l1d_hwp_type),
        replacement_policy=get_replacement_policy(args.l1d_rp_type),
    )


def build_walker_cache(args):
    return make_cache(
        size=args.walker_cache_size,
        assoc=args.walker_cache_assoc,
        tag_latency=args.walker_cache_tag_latency,
        data_latency=args.walker_cache_data_latency,
        response_latency=args.walker_cache_response_latency,
        mshrs=args.walker_cache_mshrs,
        tgts_per_mshr=args.walker_cache_tgts_per_mshr,
        replacement_policy=get_replacement_policy(args.walker_rp_type),
    )


def build_l2_cache(args, clk_domain):
    return make_cache(
        size=args.l2_size,
        assoc=args.l2_assoc,
        tag_latency=args.l2_tag_latency,
        data_latency=args.l2_data_latency,
        response_latency=args.l2_response_latency,
        mshrs=args.l2_mshrs,
        tgts_per_mshr=args.l2_tgts_per_mshr,
        write_buffers=args.l2_write_buffers,
        prefetcher=get_prefetcher(args.l2_hwp_type),
        replacement_policy=get_replacement_policy(args.l2_rp_type),
        clk_domain=clk_domain,
    )


def apply_simobject_params(simobj, param_map, args):
    for simobj_param, arg_name in param_map:
        setattr(simobj, simobj_param, getattr(args, arg_name))


def configure_cpu_o3(core, args):
    cpu_params = [
        ("cacheStorePorts", "cache_store_ports"),
        ("cacheLoadPorts", "cache_load_ports"),
        ("decodeToFetchDelay", "decode_to_fetch_delay"),
        ("renameToFetchDelay", "rename_to_fetch_delay"),
        ("iewToFetchDelay", "iew_to_fetch_delay"),
        ("commitToFetchDelay", "commit_to_fetch_delay"),
        ("fetchWidth", "fetch_width"),
        ("fetchBufferSize", "fetch_buffer_size"),
        ("fetchQueueSize", "fetch_queue_size"),
        ("renameToDecodeDelay", "rename_to_decode_delay"),
        ("iewToDecodeDelay", "iew_to_decode_delay"),
        ("commitToDecodeDelay", "commit_to_decode_delay"),
        ("fetchToDecodeDelay", "fetch_to_decode_delay"),
        ("decodeWidth", "decode_width"),
        ("iewToRenameDelay", "iew_to_rename_delay"),
        ("commitToRenameDelay", "commit_to_rename_delay"),
        ("decodeToRenameDelay", "decode_to_rename_delay"),
        ("renameWidth", "rename_width"),
        ("commitToIEWDelay", "commit_to_iew_delay"),
        ("renameToIEWDelay", "rename_to_iew_delay"),
        ("issueToExecuteDelay", "issue_to_execute_delay"),
        ("dispatchWidth", "dispatch_width"),
        ("issueWidth", "issue_width"),
        ("wbWidth", "wb_width"),
        ("iewToCommitDelay", "iew_to_commit_delay"),
        ("renameToROBDelay", "rename_to_rob_delay"),
        ("commitWidth", "commit_width"),
        ("squashWidth", "squash_width"),
        ("trapLatency", "trap_latency"),
        ("fetchTrapLatency", "fetch_trap_latency"),
        ("backComSize", "back_com_size"),
        ("forwardComSize", "forward_com_size"),
        ("LQEntries", "lq_entries"),
        ("SQEntries", "sq_entries"),
        ("LSQDepCheckShift", "lsq_dep_check_shift"),
        ("LSQCheckLoads", "lsq_check_loads"),
        ("store_set_clear_period", "store_set_clear_period"),
        ("LFSTSize", "lfst_size"),
        ("SSITSize", "ssit_size"),
        ("SSITAssoc", "ssit_assoc"),
        ("numRobs", "num_robs"),
        ("numPhysIntRegs", "num_phys_int_regs"),
        ("numPhysFloatRegs", "num_phys_float_regs"),
        ("numPhysVecRegs", "num_phys_vec_regs"),
        ("numPhysVecPredRegs", "num_phys_vec_pred_regs"),
        ("numPhysMatRegs", "num_phys_mat_regs"),
        ("numPhysCCRegs", "num_phys_cc_regs"),
        ("numIQEntries", "num_iq_entries"),
        ("numROBEntries", "num_rob_entries"),
        ("recvRespThrottling", "recv_resp_throttling"),
        ("recvRespMaxCachelines", "recv_resp_max_cachelines"),
        ("recvRespBufferSize", "recv_resp_buffer_size"),
        ("needsTSO", "needs_tso"),
    ]
    apply_simobject_params(core, cpu_params, args)


def configure_branch_predictor(core, args):
    available_bp_types = set(ObjectList.bp_list.get_names())
    if args.bp_type not in available_bp_types:
        fatal(
            f"Branch predictor '{args.bp_type}' is not available in this build. "
            "Use --list-bp-types to inspect valid names."
        )

    bp_class = ObjectList.bp_list.get(args.bp_type)
    branch_pred = bp_class()
    branch_pred.instShiftAmt = args.bp_inst_shift_amt
    branch_pred.speculativeHistUpdate = args.bp_speculative_hist_update
    branch_pred.requiresBTBHit = args.bp_requires_btb_hit
    branch_pred.takenOnlyHistory = args.bp_taken_only_history

    if hasattr(branch_pred, "btb") and branch_pred.btb not in (None, NULL):
        branch_pred.btb.numEntries = args.btb_entries
        branch_pred.btb.tagBits = args.btb_tag_bits
        branch_pred.btb.associativity = args.btb_assoc
        branch_pred.btb.instShiftAmt = args.bp_inst_shift_amt
        branch_pred.btb.btbIndexingPolicy = BTBSetAssociative(
            assoc=args.btb_assoc,
            num_entries=args.btb_entries,
            set_shift=args.bp_inst_shift_amt,
            numThreads=1,
        )
        btb_rp = get_replacement_policy(args.btb_rp_type)
        if btb_rp is not None:
            branch_pred.btb.btbReplPolicy = btb_rp

    if args.disable_ras:
        branch_pred.ras = NULL
    elif hasattr(branch_pred, "ras") and branch_pred.ras not in (None, NULL):
        branch_pred.ras.numEntries = args.ras_entries

    if args.disable_indirect_bp:
        branch_pred.indirectBranchPred = NULL
    else:
        if args.indirect_bp_type:
            available_indirect_types = set(
                ObjectList.indirect_bp_list.get_names()
            )
            if args.indirect_bp_type not in available_indirect_types:
                fatal(
                    f"Indirect predictor '{args.indirect_bp_type}' is not "
                    "available in this build. Use --list-indirect-bp-types."
                )
            branch_pred.indirectBranchPred = ObjectList.indirect_bp_list.get(
                args.indirect_bp_type
            )()

        indirect_pred = getattr(branch_pred, "indirectBranchPred", NULL)
        if indirect_pred not in (None, NULL):
            indirect_pred.indirectHashGHR = args.indirect_hash_ghr
            indirect_pred.indirectHashTargets = args.indirect_hash_targets
            indirect_pred.indirectSets = args.indirect_sets
            indirect_pred.indirectWays = args.indirect_ways
            indirect_pred.indirectTagSize = args.indirect_tag_size
            indirect_pred.indirectPathLength = args.indirect_path_length
            indirect_pred.speculativePathLength = (
                args.indirect_speculative_path_length
            )
            indirect_pred.indirectGHRBits = args.indirect_ghr_bits
            indirect_pred.instShiftAmt = args.bp_inst_shift_amt

    if hasattr(branch_pred, "localPredictorSize"):
        branch_pred.localPredictorSize = args.local_predictor_size
    if hasattr(branch_pred, "localCtrBits"):
        branch_pred.localCtrBits = args.local_ctr_bits
    if hasattr(branch_pred, "localHistoryTableSize"):
        branch_pred.localHistoryTableSize = args.local_history_table_size
    if hasattr(branch_pred, "globalPredictorSize"):
        branch_pred.globalPredictorSize = args.global_predictor_size
    if hasattr(branch_pred, "globalCtrBits"):
        branch_pred.globalCtrBits = args.global_ctr_bits
    if hasattr(branch_pred, "choicePredictorSize"):
        branch_pred.choicePredictorSize = args.choice_predictor_size
    if hasattr(branch_pred, "choiceCtrBits"):
        branch_pred.choiceCtrBits = args.choice_ctr_bits

    core.branchPred = branch_pred


def configure_cache_hierarchy(args, system):
    if args.external_memory_system and (args.caches or args.l2cache):
        fatal(
            "External memory system and internal caches are mutually exclusive."
        )

    if args.external_memory_system:
        external_cache = ExternalCacheFactory(args.external_memory_system)
    else:
        external_cache = None

    system.cache_line_size = args.cacheline_size

    if args.l2cache:
        system.l2 = build_l2_cache(args, system.cpu_clk_domain)
        system.tol2bus = L2XBar(clk_domain=system.cpu_clk_domain)
        system.l2.cpu_side = system.tol2bus.mem_side_ports
        system.l2.mem_side = system.membus.cpu_side_ports

    if args.memchecker:
        system.memchecker = MemChecker()

    for i in range(args.num_cpus):
        if args.caches:
            icache = build_l1i_cache(args)
            dcache = build_l1d_cache(args)

            if args.walker_caches:
                iwalkcache = build_walker_cache(args)
                dwalkcache = build_walker_cache(args)
            else:
                iwalkcache = None
                dwalkcache = None

            if args.memchecker:
                dcache_mon = MemCheckerMonitor(warn_only=True)
                dcache_real = dcache
                dcache_mon.memchecker = system.memchecker
                dcache_mon.mem_side = dcache.cpu_side
                dcache = dcache_mon

            system.cpu[i].addPrivateSplitL1Caches(
                icache, dcache, iwalkcache, dwalkcache
            )

            if args.memchecker:
                system.cpu[i].dcache = dcache_real
                system.cpu[i].dcache_mon = dcache_mon

        elif args.external_memory_system:
            system.cpu[i].addPrivateSplitL1Caches(
                external_cache(f"cpu{i}.icache"),
                external_cache(f"cpu{i}.dcache"),
                external_cache(f"cpu{i}.itb_walker_cache"),
                external_cache(f"cpu{i}.dtb_walker_cache"),
            )

        system.cpu[i].createInterruptController()
        if args.l2cache:
            system.cpu[i].connectAllPorts(
                system.tol2bus.cpu_side_ports,
                system.membus.cpu_side_ports,
                system.membus.mem_side_ports,
            )
        elif args.external_memory_system:
            system.cpu[i].connectUncachedPorts(
                system.membus.cpu_side_ports, system.membus.mem_side_ports
            )
        else:
            system.cpu[i].connectBus(system.membus)


def add_detailed_cache_options(parser):
    parser.add_argument(
        "--no-caches",
        dest="caches",
        action="store_false",
        help="Disable the private L1 cache hierarchy.",
    )
    parser.add_argument(
        "--no-l2cache",
        dest="l2cache",
        action="store_false",
        help="Disable the shared L2 cache.",
    )
    parser.add_argument(
        "--no-walker-caches",
        dest="walker_caches",
        action="store_false",
        help="Disable ITLB/DTLB walker caches.",
    )

    parser.add_argument(
        "--l1i-tag-latency",
        type=int,
        default=2,
        help="L1I tag latency in cycles.",
    )
    parser.add_argument(
        "--l1i-data-latency",
        type=int,
        default=2,
        help="L1I data latency in cycles.",
    )
    parser.add_argument(
        "--l1i-response-latency",
        type=int,
        default=2,
        help="L1I response latency in cycles.",
    )
    parser.add_argument(
        "--l1i-mshrs", type=int, default=4, help="L1I MSHR count."
    )
    parser.add_argument(
        "--l1i-tgts-per-mshr",
        type=int,
        default=20,
        help="Maximum targets per L1I MSHR.",
    )
    parser.add_argument(
        "--l1i-rp-type",
        default=None,
        choices=ObjectList.rp_list.get_names(),
        help="Replacement policy for the L1I cache.",
    )

    parser.add_argument(
        "--l1d-tag-latency",
        type=int,
        default=2,
        help="L1D tag latency in cycles.",
    )
    parser.add_argument(
        "--l1d-data-latency",
        type=int,
        default=2,
        help="L1D data latency in cycles.",
    )
    parser.add_argument(
        "--l1d-response-latency",
        type=int,
        default=2,
        help="L1D response latency in cycles.",
    )
    parser.add_argument(
        "--l1d-mshrs", type=int, default=4, help="L1D MSHR count."
    )
    parser.add_argument(
        "--l1d-tgts-per-mshr",
        type=int,
        default=20,
        help="Maximum targets per L1D MSHR.",
    )
    parser.add_argument(
        "--l1d-write-buffers",
        type=int,
        default=8,
        help="L1D write buffer count.",
    )
    parser.add_argument(
        "--l1d-rp-type",
        default=None,
        choices=ObjectList.rp_list.get_names(),
        help="Replacement policy for the L1D cache.",
    )

    parser.add_argument(
        "--walker-cache-size",
        type=str,
        default="1KiB",
        help="Page-table walker cache size.",
    )
    parser.add_argument(
        "--walker-cache-assoc",
        type=int,
        default=2,
        help="Page-table walker cache associativity.",
    )
    parser.add_argument(
        "--walker-cache-tag-latency",
        type=int,
        default=2,
        help="Walker cache tag latency in cycles.",
    )
    parser.add_argument(
        "--walker-cache-data-latency",
        type=int,
        default=2,
        help="Walker cache data latency in cycles.",
    )
    parser.add_argument(
        "--walker-cache-response-latency",
        type=int,
        default=2,
        help="Walker cache response latency in cycles.",
    )
    parser.add_argument(
        "--walker-cache-mshrs",
        type=int,
        default=10,
        help="Walker cache MSHR count.",
    )
    parser.add_argument(
        "--walker-cache-tgts-per-mshr",
        type=int,
        default=12,
        help="Maximum targets per walker cache MSHR.",
    )
    parser.add_argument(
        "--walker-rp-type",
        default=None,
        choices=ObjectList.rp_list.get_names(),
        help="Replacement policy for the walker caches.",
    )

    parser.add_argument(
        "--l2-tag-latency",
        type=int,
        default=20,
        help="L2 tag latency in cycles.",
    )
    parser.add_argument(
        "--l2-data-latency",
        type=int,
        default=20,
        help="L2 data latency in cycles.",
    )
    parser.add_argument(
        "--l2-response-latency",
        type=int,
        default=20,
        help="L2 response latency in cycles.",
    )
    parser.add_argument(
        "--l2-mshrs", type=int, default=20, help="L2 MSHR count."
    )
    parser.add_argument(
        "--l2-tgts-per-mshr",
        type=int,
        default=12,
        help="Maximum targets per L2 MSHR.",
    )
    parser.add_argument(
        "--l2-write-buffers",
        type=int,
        default=8,
        help="L2 write buffer count.",
    )
    parser.add_argument(
        "--l2-rp-type",
        default=None,
        choices=ObjectList.rp_list.get_names(),
        help="Replacement policy for the L2 cache.",
    )


def add_detailed_o3_options(parser):
    parser.add_argument(
        "--fetch-width", type=int, default=8, help="Fetch width."
    )
    parser.add_argument(
        "--decode-width", type=int, default=8, help="Decode width."
    )
    parser.add_argument(
        "--rename-width", type=int, default=8, help="Rename width."
    )
    parser.add_argument(
        "--dispatch-width", type=int, default=8, help="Dispatch width."
    )
    parser.add_argument(
        "--issue-width", type=int, default=8, help="Issue width."
    )
    parser.add_argument(
        "--wb-width", type=int, default=8, help="Writeback width."
    )
    parser.add_argument(
        "--commit-width", type=int, default=8, help="Commit width."
    )
    parser.add_argument(
        "--squash-width", type=int, default=8, help="Squash width."
    )

    parser.add_argument(
        "--fetch-buffer-size",
        type=int,
        default=64,
        help="Fetch buffer size in bytes.",
    )
    parser.add_argument(
        "--fetch-queue-size",
        type=int,
        default=32,
        help="Fetch queue size in micro-ops per thread.",
    )
    parser.add_argument(
        "--num-rob-entries",
        type=int,
        default=192,
        help="ROB entry count.",
    )
    parser.add_argument(
        "--num-iq-entries",
        type=int,
        default=64,
        help="Issue queue entry count.",
    )
    parser.add_argument(
        "--lq-entries", type=int, default=32, help="Load queue size."
    )
    parser.add_argument(
        "--sq-entries", type=int, default=32, help="Store queue size."
    )
    parser.add_argument(
        "--cache-load-ports",
        type=int,
        default=200,
        help="Maximum cache load ports seen by O3.",
    )
    parser.add_argument(
        "--cache-store-ports",
        type=int,
        default=200,
        help="Maximum cache store ports seen by O3.",
    )
    parser.add_argument(
        "--num-robs", type=int, default=1, help="ROB bank count."
    )

    parser.add_argument(
        "--num-phys-int-regs",
        type=int,
        default=256,
        help="Physical integer register count.",
    )
    parser.add_argument(
        "--num-phys-float-regs",
        type=int,
        default=256,
        help="Physical floating-point register count.",
    )
    parser.add_argument(
        "--num-phys-vec-regs",
        type=int,
        default=256,
        help="Physical vector register count.",
    )
    parser.add_argument(
        "--num-phys-vec-pred-regs",
        type=int,
        default=32,
        help="Physical vector predicate register count.",
    )
    parser.add_argument(
        "--num-phys-mat-regs",
        type=int,
        default=2,
        help="Physical matrix register count.",
    )
    parser.add_argument(
        "--num-phys-cc-regs",
        type=int,
        default=0,
        help="Physical condition-code register count.",
    )

    parser.add_argument(
        "--decode-to-fetch-delay",
        type=int,
        default=1,
        help="Decode to fetch delay.",
    )
    parser.add_argument(
        "--rename-to-fetch-delay",
        type=int,
        default=1,
        help="Rename to fetch delay.",
    )
    parser.add_argument(
        "--iew-to-fetch-delay",
        type=int,
        default=1,
        help="IEW to fetch delay.",
    )
    parser.add_argument(
        "--commit-to-fetch-delay",
        type=int,
        default=1,
        help="Commit to fetch delay.",
    )
    parser.add_argument(
        "--rename-to-decode-delay",
        type=int,
        default=1,
        help="Rename to decode delay.",
    )
    parser.add_argument(
        "--iew-to-decode-delay",
        type=int,
        default=1,
        help="IEW to decode delay.",
    )
    parser.add_argument(
        "--commit-to-decode-delay",
        type=int,
        default=1,
        help="Commit to decode delay.",
    )
    parser.add_argument(
        "--fetch-to-decode-delay",
        type=int,
        default=1,
        help="Fetch to decode delay.",
    )
    parser.add_argument(
        "--iew-to-rename-delay",
        type=int,
        default=1,
        help="IEW to rename delay.",
    )
    parser.add_argument(
        "--commit-to-rename-delay",
        type=int,
        default=1,
        help="Commit to rename delay.",
    )
    parser.add_argument(
        "--decode-to-rename-delay",
        type=int,
        default=1,
        help="Decode to rename delay.",
    )
    parser.add_argument(
        "--commit-to-iew-delay",
        type=int,
        default=1,
        help="Commit to IEW delay.",
    )
    parser.add_argument(
        "--rename-to-iew-delay",
        type=int,
        default=2,
        help="Rename to IEW delay.",
    )
    parser.add_argument(
        "--issue-to-execute-delay",
        type=int,
        default=1,
        help="Issue to execute delay.",
    )
    parser.add_argument(
        "--iew-to-commit-delay",
        type=int,
        default=1,
        help="IEW to commit delay.",
    )
    parser.add_argument(
        "--rename-to-rob-delay",
        type=int,
        default=1,
        help="Rename to ROB delay.",
    )
    parser.add_argument(
        "--trap-latency", type=int, default=13, help="Trap latency in cycles."
    )
    parser.add_argument(
        "--fetch-trap-latency",
        type=int,
        default=1,
        help="Fetch trap latency in cycles.",
    )
    parser.add_argument(
        "--back-com-size",
        type=int,
        default=5,
        help="Backward communication time-buffer size.",
    )
    parser.add_argument(
        "--forward-com-size",
        type=int,
        default=5,
        help="Forward communication time-buffer size.",
    )

    parser.add_argument(
        "--lsq-dep-check-shift",
        type=int,
        default=4,
        help="Address shift applied before LSQ dependency checks.",
    )
    parser.add_argument(
        "--disable-lsq-check-loads",
        dest="lsq_check_loads",
        action="store_false",
        help="Disable LSQ dependency checks for loads.",
    )
    parser.add_argument(
        "--store-set-clear-period",
        type=int,
        default=250000,
        help="Store-set predictor invalidation period.",
    )
    parser.add_argument(
        "--lfst-size",
        type=int,
        default=1024,
        help="LFST size.",
    )
    parser.add_argument(
        "--ssit-size",
        type=str,
        default="1024",
        help="SSIT size.",
    )
    parser.add_argument(
        "--ssit-assoc",
        type=int,
        default=1,
        help="SSIT associativity.",
    )

    parser.add_argument(
        "--enable-recv-resp-throttling",
        dest="recv_resp_throttling",
        action="store_true",
        help="Enable LSQ response throttling.",
    )
    parser.add_argument(
        "--recv-resp-max-cachelines",
        type=int,
        default=1,
        help="Maximum receive-response cache lines per cycle.",
    )
    parser.add_argument(
        "--recv-resp-buffer-size",
        type=int,
        default=64,
        help="Receive-response buffer size in bytes.",
    )
    parser.add_argument(
        "--needs-tso",
        action="store_true",
        help="Enable TSO memory model for the O3 core.",
    )


def add_detailed_predictor_options(parser):
    parser.add_argument(
        "--bp-inst-shift-amt",
        type=int,
        default=2,
        help="Instruction shift amount used by the predictor frontend.",
    )
    parser.add_argument(
        "--bp-speculative-hist-update",
        dest="bp_speculative_hist_update",
        action="store_true",
        help="Enable speculative history updates in the branch predictor.",
    )
    parser.add_argument(
        "--no-bp-speculative-hist-update",
        dest="bp_speculative_hist_update",
        action="store_false",
        help="Disable speculative history updates in the branch predictor.",
    )
    parser.add_argument(
        "--bp-requires-btb-hit",
        action="store_true",
        help="Require a BTB hit for returns and indirect branches.",
    )
    parser.add_argument(
        "--bp-taken-only-history",
        action="store_true",
        help="Build global history using only taken branches.",
    )

    parser.add_argument(
        "--btb-entries", type=int, default=4096, help="BTB entries."
    )
    parser.add_argument(
        "--btb-tag-bits", type=int, default=16, help="BTB tag width in bits."
    )
    parser.add_argument(
        "--btb-assoc", type=int, default=1, help="BTB associativity."
    )
    parser.add_argument(
        "--btb-rp-type",
        default=None,
        choices=ObjectList.rp_list.get_names(),
        help="Replacement policy for the BTB.",
    )

    parser.add_argument(
        "--ras-entries",
        type=int,
        default=16,
        help="Return-address stack depth.",
    )
    parser.add_argument(
        "--disable-ras",
        action="store_true",
        help="Disable the return-address stack.",
    )

    parser.add_argument(
        "--disable-indirect-bp",
        action="store_true",
        help="Disable the indirect branch predictor.",
    )
    parser.add_argument(
        "--indirect-hash-ghr",
        dest="indirect_hash_ghr",
        action="store_true",
        help="Hash the indirect predictor GHR.",
    )
    parser.add_argument(
        "--no-indirect-hash-ghr",
        dest="indirect_hash_ghr",
        action="store_false",
        help="Disable GHR hashing in the indirect predictor.",
    )
    parser.add_argument(
        "--indirect-hash-targets",
        dest="indirect_hash_targets",
        action="store_true",
        help="Hash path targets in the indirect predictor.",
    )
    parser.add_argument(
        "--no-indirect-hash-targets",
        dest="indirect_hash_targets",
        action="store_false",
        help="Disable path-target hashing in the indirect predictor.",
    )
    parser.add_argument(
        "--indirect-sets",
        type=int,
        default=256,
        help="Indirect predictor sets.",
    )
    parser.add_argument(
        "--indirect-ways", type=int, default=2, help="Indirect predictor ways."
    )
    parser.add_argument(
        "--indirect-tag-size",
        type=int,
        default=16,
        help="Indirect predictor tag width in bits.",
    )
    parser.add_argument(
        "--indirect-path-length",
        type=int,
        default=3,
        help="Indirect predictor path length.",
    )
    parser.add_argument(
        "--indirect-speculative-path-length",
        type=int,
        default=256,
        help="Indirect predictor speculative path history size.",
    )
    parser.add_argument(
        "--indirect-ghr-bits",
        type=int,
        default=13,
        help="Indirect predictor GHR width.",
    )

    parser.add_argument(
        "--local-predictor-size",
        type=int,
        default=2048,
        help="Local predictor table size.",
    )
    parser.add_argument(
        "--local-ctr-bits",
        type=int,
        default=2,
        help="Local predictor counter width.",
    )
    parser.add_argument(
        "--local-history-table-size",
        type=int,
        default=2048,
        help="TournamentBP local history table size.",
    )
    parser.add_argument(
        "--global-predictor-size",
        type=int,
        default=8192,
        help="Global predictor table size.",
    )
    parser.add_argument(
        "--global-ctr-bits",
        type=int,
        default=2,
        help="Global predictor counter width.",
    )
    parser.add_argument(
        "--choice-predictor-size",
        type=int,
        default=8192,
        help="Choice predictor table size.",
    )
    parser.add_argument(
        "--choice-ctr-bits",
        type=int,
        default=2,
        help="Choice predictor counter width.",
    )


parser = argparse.ArgumentParser()
Options.addCommonOptions(parser, ISA.RISCV)
Options.addFSOptions(parser)
parser.add_argument(
    "--virtio-rng", action="store_true", help="Enable VirtIORng device."
)
parser.add_argument(
    "--semihosting",
    action="store_true",
    help="Enable the RISC-V semihosting interface.",
)
parser.add_argument(
    "--semihosting-root",
    default="/some/invalid/root/directory",
    type=str,
    help="The root directory for files exposed to semihosting.",
)
parser.add_argument(
    "--riscv-32bits",
    action="store_true",
    help="Use a 32-bit RISC-V core.",
)

add_detailed_cache_options(parser)
add_detailed_o3_options(parser)
add_detailed_predictor_options(parser)

parser.set_defaults(
    cpu_type=DEFAULT_O3_CPU_TYPE,
    caches=True,
    l2cache=True,
    walker_caches=True,
    lsq_check_loads=True,
    recv_resp_throttling=False,
    bp_type=DEFAULT_BP_TYPE,
    indirect_bp_type=DEFAULT_INDIRECT_BP_TYPE,
    bp_speculative_hist_update=True,
    indirect_hash_ghr=True,
    indirect_hash_targets=True,
)

args = parser.parse_args()

if not args.kernel:
    parser.error("--kernel argument is required")

if args.num_l2caches != 1:
    warn(
        "This config models a single shared L2 cache; --num-l2caches is ignored."
    )
if args.num_l3caches != 1 or args.l3_size != "16MiB" or args.l3_assoc != 16:
    warn("L3 cache options are ignored by this config.")
if args.num_dirs != 1:
    warn("Directory options are ignored because Ruby is unsupported here.")

if args.ruby:
    fatal("Ruby is not supported by this detailed RISC-V FS config.")

(CPUClass, mem_mode, FutureClass) = Simulation.setCPUClass(args)

initial_cpu_is_o3 = cpu_is_o3(CPUClass)
future_cpu_is_o3 = cpu_is_o3(FutureClass)

if not initial_cpu_is_o3 and not future_cpu_is_o3:
    warn(
        "Running this detailed config without an O3 CPU active in either the "
        "initial or switched phase. O3-specific width/queue/rename options "
        "will be ignored."
    )
if ObjectList.cpu_list.get_isa(args.cpu_type) != ISA.RISCV:
    fatal("This config only supports RISC-V CPUs.")

if (initial_cpu_is_o3 or future_cpu_is_o3) and not (
    args.caches or args.external_memory_system
):
    fatal("RiscvO3CPU requires caches or an external memory system.")

if args.l2cache and not args.caches:
    fatal("Shared L2 cache requires private L1 caches to be enabled.")

np = args.num_cpus

system = System()
mdesc = SysConfig(
    disks=args.disk_image,
    rootdev=args.root_device,
    mem=args.mem_size,
    os_type=args.os_type,
)
system.mem_mode = mem_mode
system.mem_ranges = [AddrRange(start=0x80000000, size=mdesc.mem())]

workload_args = {}
if args.semihosting:
    workload_args["semihosting"] = RiscvSemihosting(
        files_root_dir=args.semihosting_root,
        cmd_line=args.kernel,
    )
if args.bare_metal:
    system.workload = RiscvBareMetal(**workload_args)
    system.workload.bootloader = args.kernel
elif not args.bootloader:
    system.workload = RiscvLinux(**workload_args)
    system.workload.object_file = args.kernel
else:
    system.workload = RiscvBootloaderKernelWorkload(**workload_args)
    system.workload.bootloader_filename = args.bootloader
    system.workload.object_file = args.kernel

system.iobus = IOXBar()
system.membus = MemBus()
system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency=Frequency("100MHz"))
system.platform.clint.int_pin = system.platform.rtc.int_pin
system.platform.pci_host.pio = system.iobus.mem_side_ports

if args.disk_image:
    image = CowDiskImage(child=RawDiskImage(read_only=True), read_only=False)
    image.child.image_file = mdesc.disks()[0]
    system.platform.disk = RiscvMmioVirtIO(
        vio=VirtIOBlock(image=image),
        interrupt_id=0x8,
        pio_size=4096,
        pio_addr=0x10008000,
    )

if args.virtio_rng:
    system.platform.rng = RiscvMmioVirtIO(
        vio=VirtIORng(), interrupt_id=0x8, pio_size=4096, pio_addr=0x10007000
    )

system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports
system.bridge.ranges = system.platform._off_chip_ranges()

system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(np)

system.cache_line_size = args.cacheline_size
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock=args.cpu_clock, voltage_domain=system.cpu_voltage_domain
)

if args.script is not None:
    system.readfile = args.script

system.init_param = args.init_param
system.cpu = [
    CPUClass(clk_domain=system.cpu_clk_domain, cpu_id=i) for i in range(np)
]

for core in system.cpu:
    if args.riscv_32bits:
        core.ArchISA.riscv_type = "RV32"
    configure_cpu_instance(core, args)

if future_cpu_is_o3:
    for i in range(np):
        if args.riscv_32bits:
            system.cpu[i].ArchISA.riscv_type = "RV32"

    def configure_switch_cpu(core, _cpu_id):
        if args.riscv_32bits:
            core.ArchISA.riscv_type = "RV32"
        configure_cpu_instance(core, args)

    args.configure_switch_cpu = configure_switch_cpu
else:
    args.configure_switch_cpu = None

if args.caches or args.l2cache:
    system.iocache = IOCache(addr_ranges=system.mem_ranges)
    system.iocache.cpu_side = system.iobus.mem_side_ports
    system.iocache.mem_side = system.membus.cpu_side_ports
elif not args.external_memory_system:
    system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
    system.iobridge.cpu_side_port = system.iobus.mem_side_ports
    system.iobridge.mem_side_port = system.membus.cpu_side_ports

if args.simpoint_profile:
    fatal("SimPoint generation should be done with an atomic CPU.")

for core in system.cpu:
    core.createThreads()

uncacheable_range = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
for cpu in system.cpu:
    cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

configure_cache_hierarchy(args, system)

if not args.bare_metal:
    system.workload.dtb_addr = 0x87E00000
    if args.command_line:
        system.workload.command_line = args.command_line
    else:
        kernel_cmd = ["console=ttyS0", "root=/dev/vda", "ro"]
        system.workload.command_line = " ".join(kernel_cmd)

    if args.dtb_filename:
        system.workload.dtb_filename = args.dtb_filename
    else:
        generateDtb(system)
        system.workload.dtb_filename = path.join(
            m5.options.outdir, "device.dtb"
        )

if (
    args.elastic_trace_en
    and args.checkpoint_restore is None
    and not args.fast_forward
    and initial_cpu_is_o3
):
    CpuConfig.config_etrace(CPUClass, system.cpu, args)

MemConfig.config_mem(args, system)

root = Root(full_system=True, system=system)
Simulation.setWorkCountOptions(system, args)
Simulation.run(args, root, system, FutureClass)
