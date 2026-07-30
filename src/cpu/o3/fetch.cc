/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/DecoupledBPU.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

namespace
{

bool
decoupledBPUTypeMatches(const DynInstPtr &inst, cfi_type_t type)
{
    switch (type) {
      case CFI_BRA:
        return inst->isDirectCtrl() && inst->isCondCtrl();
      case CFI_JAL:
        return inst->isDirectCtrl() && inst->isUncondCtrl();
      case CFI_JALR_CALL:
        return inst->isIndirectCtrl() && !inst->isReturn();
      case CFI_JALR_RET:
        return inst->isReturn();
      case CFI_NULL:
        return false;
    }

    return false;
}

cfi_type_t
decoupledBPUCFIType(const DynInstPtr &inst)
{
    if (!inst || !inst->isControl()) {
        return CFI_NULL;
    }

    if (inst->isReturn()) {
        return CFI_JALR_RET;
    }

    if (inst->isIndirectCtrl()) {
        return CFI_JALR_CALL;
    }

    if (inst->isDirectCtrl() && inst->isCondCtrl()) {
        return CFI_BRA;
    }

    if (inst->isDirectCtrl() && inst->isUncondCtrl()) {
        return CFI_JAL;
    }

    return CFI_NULL;
}

const char*
decoupledBPUCFITypeName(cfi_type_t type)
{
    switch (type) {
      case CFI_BRA:
        return "bra";
      case CFI_JAL:
        return "jal";
      case CFI_JALR_CALL:
        return "jalr_call";
      case CFI_JALR_RET:
        return "jalr_ret";
      case CFI_NULL:
        return "none";
    }

    return "unknown";
}

} // namespace

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port"), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      branchPred(nullptr),
      decoupledBPUEnabled(params.decoupledBPU),
      decoupledBPUUseTAGE(params.decoupledBPUUseTAGE),
      decoupledBPUUseRAS(params.decoupledBPUUseRAS),
      decoupledBPUUseITTAGE(params.decoupledBPUUseITTAGE),
      decoupledBPUBurstTicks(std::max(1U, params.decoupledBPUBurstTicks)),
      decoupledBPUBanks(params.decoupledBPUBanks),
      decoupledBPUFTQDepth(params.decoupledBPUFTQDepth),
      decoupledBPUUBTBEntries(params.decoupledBPUUBTBEntries),
      decoupledBPUBTBWays(params.decoupledBPUBTBWays),
      decoupledBPUBTBSets(params.decoupledBPUBTBSets),
      decoupledBPUTTWays(params.decoupledBPUTTWays),
      decoupledBPUTTSets(params.decoupledBPUTTSets),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(NULL),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      finishTranslationEvent(this), fetchStats(_cpu, this)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    if (fetchBufferSize > cacheBlkSize)
        fatal("fetch buffer size (%u bytes) is greater than the cache "
              "block size (%u bytes)\n", fetchBufferSize, cacheBlkSize);
    if (cacheBlkSize % fetchBufferSize)
        fatal("cache block (%u bytes) is not a multiple of the "
              "fetch buffer (%u bytes)\n", cacheBlkSize, fetchBufferSize);

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        fetchOffset[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        memReq[i] = nullptr;
        stalls[i] = {false, false};
        fetchBuffer[i] = NULL;
        fetchBufferPC[i] = 0;
        fetchBufferValid[i] = false;
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
        decoupledBPULastSpeculativeId[i] = -1;
        decoupledBPUFtqFullLastCycle[i] = false;
        decoupledBPUJalrStall[i] = false;
        decoupledBPUJalrStallSeqNum[i] = 0;
        decoupledBPUJalrStallPC[i] = 0;
    }

    branchPred = params.branchPred;

    if (decoupledBPUEnabled) {
        decoupledBPUBanks = std::max(1U, decoupledBPUBanks);
        if (bpu_cfg::fetch_block_2b_count % decoupledBPUBanks != 0) {
            fatal("decoupledBPUBanks (%u) must divide fetch block halfword "
                  "count (%d)\n",
                  decoupledBPUBanks, bpu_cfg::fetch_block_2b_count);
        }
    }

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Create space to buffer the cache line data,
        // which may not hold the entire cache line.
        fetchBuffer[tid] = new uint8_t[fetchBufferSize];

        if (decoupledBPUEnabled) {
            ubtb_cfg ubtbCfg;
            ubtbCfg.way_count = std::max(1U, decoupledBPUUBTBEntries);
            ubtbCfg.tag_width = 16;
            ubtbCfg.tag_pc_shift = 1;

            btb_cfg btbCfg;
            btbCfg.way_count = std::max(1U, decoupledBPUBTBWays);
            btbCfg.set_count = std::max(1U, decoupledBPUBTBSets);
            btbCfg.bank_count = decoupledBPUBanks;
            btbCfg.tag_width = 16;
            btbCfg.tag_pc_shift = 1;

            tt_cfg ttCfg;
            ttCfg.way_count = std::max(1U, decoupledBPUTTWays);
            ttCfg.set_count = std::max(1U, decoupledBPUTTSets);
            ttCfg.bank_count = decoupledBPUBanks;
            ttCfg.tag_width = 16;
            ttCfg.tag_pc_shift = 1;

            ftq_cfg ftqCfg;
            ftqCfg.depth = std::max(1U, decoupledBPUFTQDepth);

            decoupledBpus[tid] =
                std::make_unique<::bpu>(
                    btbCfg, ubtbCfg, ttCfg, ftqCfg,
                    decoupledBPUUseTAGE, decoupledBPUUseRAS,
                    decoupledBPUUseITTAGE);
            decoupledBPUTracers[tid] = std::make_unique<::cfi_tracer>(
                std::max(64U, decoupledBPUFTQDepth * 4),
                decoupledBPUBanks);
        }
    }

    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(decoupledBpuTicks, statistics::units::Count::get(),
             "Number of experimental decoupled BPU ticks"),
    ADD_STAT(decoupledBpuBursts, statistics::units::Count::get(),
             "Number of demand-driven experimental decoupled BPU bursts"),
    ADD_STAT(decoupledBpuSpeculativeNodesMax,
             statistics::units::Count::get(),
             "Maximum live experimental BPU speculative nodes observed"),
    ADD_STAT(decoupledBpuRASDepthMax, statistics::units::Count::get(),
             "Maximum experimental BPU RAS depth observed"),
    ADD_STAT(decoupledBpuTageCheckpointsMax,
             statistics::units::Count::get(),
             "Maximum live experimental BPU TAGE checkpoints observed"),
    ADD_STAT(decoupledBpuITTAGECheckpointsMax,
             statistics::units::Count::get(),
             "Maximum live experimental BPU ITTAGE checkpoints observed"),
    ADD_STAT(decoupledBpuFtqEmptyOnRequest, statistics::units::Count::get(),
             "Number of fetch requests that found the experimental FTQ empty"),
    ADD_STAT(decoupledBpuFtqFullOnTick, statistics::units::Count::get(),
             "Number of experimental BPU ticks that found the FTQ full"),
    ADD_STAT(decoupledBpuFtqPushes, statistics::units::Count::get(),
             "Number of entries pushed into the experimental FTQ"),
    ADD_STAT(decoupledBpuFtqConsumes, statistics::units::Count::get(),
             "Number of experimental FTQ spans consumed by fetch buffers"),
    ADD_STAT(decoupledBpuUbtbHits, statistics::units::Count::get(),
             "Experimental uBTB taken-CFI answers verified by BTB"),
    ADD_STAT(decoupledBpuUbtbMisses, statistics::units::Count::get(),
             "Experimental uBTB missing or mismatching BTB-fillable "
             "taken CFIs"),
    ADD_STAT(decoupledBpuBtbHits, statistics::units::Count::get(),
             "Experimental BTB hits for decoded taken CFIs"),
    ADD_STAT(decoupledBpuBtbMisses, statistics::units::Count::get(),
             "Experimental BTB misses for decoded taken CFIs"),
    ADD_STAT(decoupledBpuTageHits, statistics::units::Count::get(),
             "Experimental TAGE-side branch direction lookups used"),
    ADD_STAT(decoupledBpuTageMisses, statistics::units::Count::get(),
             "Experimental TAGE-side branch direction lookups bypassed"),
    ADD_STAT(decoupledBpuTTHits, statistics::units::Count::get(),
             "Experimental target table hits"),
    ADD_STAT(decoupledBpuTTMisses, statistics::units::Count::get(),
             "Experimental target table misses"),
    ADD_STAT(decoupledBpuRASHits, statistics::units::Count::get(),
             "Experimental RAS target hits"),
    ADD_STAT(decoupledBpuRASMisses, statistics::units::Count::get(),
             "Experimental RAS target misses"),
    ADD_STAT(decoupledBpuITTAGEHits, statistics::units::Count::get(),
             "Experimental ITTAGE target hits"),
    ADD_STAT(decoupledBpuITTAGEMisses, statistics::units::Count::get(),
             "Experimental ITTAGE target misses"),
    ADD_STAT(decoupledBpuUbtbFills, statistics::units::Count::get(),
             "Experimental uBTB fills from strongly-taken BTB entries"),
    ADD_STAT(decoupledBpuCfiRecords, statistics::units::Count::get(),
             "Experimental CFI records queued for BPU commit"),
    ADD_STAT(decoupledBpuBtbCommitInserts, statistics::units::Count::get(),
             "Experimental BTB entries inserted or refreshed at commit"),
    ADD_STAT(decoupledBpuBtbCtrUpdates, statistics::units::Count::get(),
             "Experimental BTB branch counters updated at commit"),
    ADD_STAT(decoupledBpuTTCommitUpdates, statistics::units::Count::get(),
             "Experimental TT entries updated at commit"),
    ADD_STAT(decoupledBpuDecodeRedirects, statistics::units::Count::get(),
             "Experimental BPU recoveries caused by decode redirects"),
    ADD_STAT(decoupledBpuBackendRedirects, statistics::units::Count::get(),
             "Experimental BPU recoveries caused by backend redirects"),
    ADD_STAT(decoupledBpuFetchPredecodeRedirects,
             statistics::units::Count::get(),
             "Experimental BPU recoveries caused by fetch predecode checks"),
    ADD_STAT(decoupledBpuDecodeDirectCondRedirects,
             statistics::units::Count::get(),
             "Decode redirects caused by conditional direct controls"),
    ADD_STAT(decoupledBpuDecodeDirectUncondRedirects,
             statistics::units::Count::get(),
             "Decode redirects caused by unconditional direct controls"),
    ADD_STAT(decoupledBpuDecodeIndirectRedirects,
             statistics::units::Count::get(),
             "Decode redirects caused by indirect controls"),
    ADD_STAT(decoupledBpuDecodeReturnRedirects,
             statistics::units::Count::get(),
             "Decode redirects caused by returns"),
    ADD_STAT(decoupledBpuDecodeOtherRedirects, statistics::units::Count::get(),
             "Decode redirects caused by other instructions"),
    ADD_STAT(decoupledBpuBackendDirectCondRedirects,
             statistics::units::Count::get(),
             "Backend redirects caused by conditional direct controls"),
    ADD_STAT(decoupledBpuBackendDirectUncondRedirects,
             statistics::units::Count::get(),
             "Backend redirects caused by unconditional direct controls"),
    ADD_STAT(decoupledBpuBackendIndirectRedirects,
             statistics::units::Count::get(),
             "Backend redirects caused by indirect controls"),
    ADD_STAT(decoupledBpuBackendReturnRedirects,
             statistics::units::Count::get(),
             "Backend redirects caused by returns"),
    ADD_STAT(decoupledBpuBackendOtherRedirects,
             statistics::units::Count::get(),
             "Backend redirects caused by other instructions"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles)
{
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
}
void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);
}

void
Fetch::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();
}

void
Fetch::clearStates(ThreadID tid)
{
    fetchStatus[tid] = Running;
    set(pc[tid], cpu->pcState(tid));
    fetchOffset[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid] = NULL;
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBufferPC[tid] = 0;
    fetchBufferValid[tid] = false;
    fetchQueue[tid].clear();
    decoupledBPULastSpeculativeId[tid] = -1;
    decoupledBPUFtqFullLastCycle[tid] = false;
    decoupledBPUJalrStall[tid] = false;
    decoupledBPUJalrStallSeqNum[tid] = 0;
    decoupledBPUJalrStallPC[tid] = 0;
    clearDecoupledBPUFetchPredictions(tid);
    decoupledBPUPendingCommits[tid].clear();
    if (decoupledBPUTracers[tid]) {
        decoupledBPUTracers[tid]->clear();
    }
    if (decoupledBPUEnabled && decoupledBpus[tid]) {
        decoupledBpus[tid]->recover(pc[tid]->instAddr());
    }

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);

    // Clear out any of this thread's instructions being sent to decode.
    for (int i = -cpu->fetchQueue.getPast();
         i <= cpu->fetchQueue.getFuture(); ++i) {
        FetchStruct& fetch_struct = cpu->fetchQueue[i];
        removeCommThreadInsts(tid, fetch_struct);
    }
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        fetchStatus[tid] = Running;
        set(pc[tid], cpu->pcState(tid));
        fetchOffset[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid] = NULL;

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBufferPC[tid] = 0;
        fetchBufferValid[tid] = false;

        fetchQueue[tid].clear();
        decoupledBPULastSpeculativeId[tid] = -1;
        decoupledBPUFtqFullLastCycle[tid] = false;
        decoupledBPUJalrStall[tid] = false;
        decoupledBPUJalrStallSeqNum[tid] = 0;
        decoupledBPUJalrStallPC[tid] = 0;
        clearDecoupledBPUFetchPredictions(tid);
        decoupledBPUPendingCommits[tid].clear();
        if (decoupledBPUTracers[tid]) {
            decoupledBPUTracers[tid]->clear();
        }
        if (decoupledBPUEnabled && decoupledBpus[tid]) {
            decoupledBpus[tid]->recover(pc[tid]->instAddr());
        }

        priorityList.push_back(tid);
    }

    wroteToTimeBuffer = false;
    _status = Inactive;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());

    DPRINTF(Fetch, "[tid:%i] Waking up from cache miss.\n", tid);
    assert(!cpu->switchedOut());

    // Only change the status if it's still waiting on the icache access
    // to return.
    if (fetchStatus[tid] != IcacheWaitResponse ||
        pkt->req != memReq[tid]) {
        ++fetchStats.icacheSquashes;
        delete pkt;
        return;
    }

    memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);
    fetchBufferValid[tid] = true;

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Only switch to IcacheAccessComplete if we're not stalled as well.
    if (checkStall(tid)) {
        fetchStatus[tid] = Blocked;
    } else {
        fetchStatus[tid] = IcacheAccessComplete;
    }

    pkt->req->setAccessLatency();
    cpu->ppInstAccessComplete->notify(pkt);
    // Reset the mem req to NULL.
    delete pkt;
    memReq[tid] = NULL;
}

void
Fetch::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].decode = false;
        stalls[i].drain = false;
    }
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt == NULL);
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(!memReq[i]);
        assert(fetchStatus[i] == Idle || stalls[i].drain);
    }

    branchPred->drainSanityCheck();
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            if (fetchStatus[i] == Blocked && stalls[i].drain)
                continue;
            else
                return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !finishTranslationEvent.scheduled();
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    fetchStatus[0] = Running;
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken;

    if (!inst->isControl()) {
        inst->staticInst->advancePC(next_pc);
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    ThreadID tid = inst->threadNumber;
    predict_taken = branchPred->predict(inst->staticInst, inst->seqNum,
                                        next_pc, tid);
    bool decoupled_predict_taken = false;
    if (applyDecoupledBPUPrediction(inst, next_pc,
                                    decoupled_predict_taken)) {
        predict_taken = decoupled_predict_taken;
    }

    if (predict_taken) {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be taken to %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    } else {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be not taken\n",
                tid, inst->seqNum, inst->pcState().instAddr());
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
            "predicted to go to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    inst->setPredTarg(next_pc);
    inst->setPredTaken(predict_taken);

    cpu->fetchStats[tid]->numBranches++;

    if (predict_taken) {
        ++fetchStats.predictedBranches;
    }

    return predict_taken;
}

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    Fault fault = NoFault;

    assert(!cpu->switchedOut());

    // @todo: not sure if these should block translation.
    //AlphaDep
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n",
                tid);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n",
                tid);
        return false;
    }

    // Align the fetch address to the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(vaddr);

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x\n",
            tid, fetchBufferBlockPC, vaddr);

    // Setup the memReq to do a read of the first instruction's address.
    // Set the appropriate read size and flags as well.
    // Build request here.
    RequestPtr mem_req = std::make_shared<Request>(
        fetchBufferBlockPC, fetchBufferSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    mem_req->taskId(cpu->taskId());

    memReq[tid] = mem_req;

    // Initiate translation of the icache block
    fetchStatus[tid] = ItlbWait;
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);
    return true;
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    Addr fetchBufferBlockPC = mem_req->getVaddr();

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    if (fetchStatus[tid] != ItlbWait || mem_req != memReq[tid] ||
        mem_req->getVaddr() != memReq[tid]->getVaddr()) {
        DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash\n",
                tid);
        ++fetchStats.tlbSquashes;
        return;
    }


    // If translation was successful, attempt to read the icache block.
    if (fault == NoFault) {
        // Check that we're not going off into random memory
        // If we have, just wait around for commit to squash something and put
        // us on the right track
        if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
            warn("Address %#x is outside of physical memory, stopping fetch\n",
                    mem_req->getPaddr());
            fetchStatus[tid] = NoGoodAddr;
            memReq[tid] = NULL;
            return;
        }

        // Build packet here.
        PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
        data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);

        fetchBufferPC[tid] = fetchBufferBlockPC;
        fetchBufferValid[tid] = false;
        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

        fetchStats.cacheLines++;

        // Access the cache.
        if (!icachePort.sendTimingReq(data_pkt)) {
            assert(retryPkt == NULL);
            assert(retryTid == InvalidThreadID);
            DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

            fetchStatus[tid] = IcacheWaitRetry;
            retryPkt = data_pkt;
            retryTid = tid;
            cacheBlocked = true;
        } else {
            DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
            DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                    "response.\n", tid);
            lastIcacheStall[tid] = curTick();
            fetchStatus[tid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a packet containing a fetch
            // request is successfully sent
            ppFetchRequestSent->notify(mem_req);
        }
    } else {
        // Don't send an instruction to decode if we can't handle it.
        if (!(numInst < fetchWidth) ||
                !(fetchQueue[tid].size() < fetchQueueSize)) {
            assert(!finishTranslationEvent.scheduled());
            finishTranslationEvent.setFault(fault);
            finishTranslationEvent.setReq(mem_req);
            cpu->schedule(finishTranslationEvent,
                          cpu->clockEdge(Cycles(1)));
            return;
        }
        DPRINTF(Fetch,
                "[tid:%i] Got back req with addr %#x but expected %#x\n",
                tid, mem_req->getVaddr(), memReq[tid]->getVaddr());
        // Translation faulted, icache request won't be sent.
        memReq[tid] = NULL;

        // Send the fault to commit.  This thread will not do anything
        // until commit handles the fault.  The only other way it can
        // wake up is if a squash comes along and changes the PC.
        const PCStateBase &fetch_pc = *pc[tid];

        DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
        // We will use a nop in ordier to carry the fault.
        DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
                fetch_pc, fetch_pc, false);
        instruction->setNotAnInst();

        instruction->setPredTarg(fetch_pc);
        instruction->fault = fault;
        wroteToTimeBuffer = true;

        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();

        fetchStatus[tid] = TrapPending;

        DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
        DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
                tid, fault->name(), *pc[tid]);
    }
    _status = updateFetchStatus();
}

void
Fetch::doSquash(const PCStateBase &new_pc, const DynInstPtr squashInst,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s.\n",
            tid, new_pc);

    set(pc[tid], new_pc);
    fetchOffset[tid] = 0;
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr() &&
        !squashInst->isLastMicroop())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    if (fetchStatus[tid] == IcacheWaitResponse) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding Icache miss.\n",
                tid);
        memReq[tid] = NULL;
    } else if (fetchStatus[tid] == ItlbWait) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding ITLB miss.\n",
                tid);
        memReq[tid] = NULL;
    }

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        if (retryPkt) {
            delete retryPkt;
        }
        retryPkt = NULL;
        retryTid = InvalidThreadID;
    }

    fetchStatus[tid] = Squashing;

    // Empty fetch queue
    fetchQueue[tid].clear();
    clearDecoupledBPUFetchPredictions(tid);

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    ++fetchStats.squashCycles;
}

void
Fetch::squashFromDecode(const PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

bool
Fetch::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(Fetch,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    for (ThreadID tid : *activeThreads) {
        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == Squashing ||
            fetchStatus[tid] == IcacheAccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (fetchStatus[tid] == IcacheAccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

bpu_cycle_input_t
Fetch::makeDecoupledBPUInput(ThreadID tid, Addr base) const
{
    (void)tid;
    (void)base;

    bpu_cycle_input_t input;
    // TODO: gem5'in legacy TAGE/ITTAGE SimObject'leri sonradan burada
    // external response olarak köprülenebilir. Şimdilik seçilen modüller
    // BPU nesnesinin içindeki checkpoint'li local predictor yolunu açar.
    input.use_tage = decoupledBPUUseTAGE;
    input.use_ras = decoupledBPUUseRAS;
    input.use_ittage = decoupledBPUUseITTAGE;

    return input;
}

void
Fetch::sampleDecoupledBPUOutput(ThreadID tid,
                                const bpu_cycle_output_t &output)
{
    (void)tid;

    const bool ubtb_attempt = output.bpu1.valid && output.bpu1.taken;
    const bool btb_verifies_ubtb =
        output.bpu2.valid && output.bpu2.taken && output.bpu2.ubtb_fillable;
    const auto same_sign = [](const bpu_sign_t& lhs,
                              const bpu_sign_t& rhs) {
        return lhs.offset == rhs.offset &&
            lhs.type == rhs.type &&
            lhs.compressed == rhs.compressed &&
            lhs.is_call == rhs.is_call;
    };
    const bool ubtb_matches_btb =
        ubtb_attempt && btb_verifies_ubtb &&
        output.bpu1.target == output.bpu2.target &&
        output.bpu1.next_cfi_span_2b == output.bpu2.next_cfi_span_2b &&
        same_sign(output.bpu1.sign, output.bpu2.sign);

    if (ubtb_matches_btb) {
        ++fetchStats.decoupledBpuUbtbHits;
    } else if (ubtb_attempt || btb_verifies_ubtb) {
        // uBTB is a fast cache of BTB's taken direct answer. Count only the
        // comparable cases: BTB has a fillable taken answer, or uBTB tried to
        // steer fetch and BTB did not verify the same answer.
        ++fetchStats.decoupledBpuUbtbMisses;
    }

    if (output.bpu2.valid && output.bpu2.sign.type == CFI_BRA) {
        if (output.bpu2.tage_used) {
            ++fetchStats.decoupledBpuTageHits;
        } else {
            ++fetchStats.decoupledBpuTageMisses;
        }
    }

    if (output.bpu2.valid && output.bpu2.sign.type == CFI_JALR_CALL) {
        if (output.bpu2.tt_hit) {
            ++fetchStats.decoupledBpuTTHits;
        } else {
            ++fetchStats.decoupledBpuTTMisses;
        }

        if (decoupledBPUUseITTAGE &&
            output.ittage_response.checkpoint_id >= 0) {
            if (output.ittage_response.hit) {
                ++fetchStats.decoupledBpuITTAGEHits;
            } else {
                ++fetchStats.decoupledBpuITTAGEMisses;
            }
        }
    }

    if (output.bpu2.valid && output.bpu2.sign.type == CFI_JALR_RET) {
        if (output.bpu2.ras_valid) {
            ++fetchStats.decoupledBpuRASHits;
        } else {
            ++fetchStats.decoupledBpuRASMisses;
        }
    }

    if (output.ftq_pushed) {
        ++fetchStats.decoupledBpuFtqPushes;
    }

    if (output.ubtb_filled) {
        ++fetchStats.decoupledBpuUbtbFills;
    }
}

void
Fetch::recordDecoupledBPUCommitInfo(ThreadID tid, const DynInstPtr &inst)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid] ||
        !decoupledBPUTracers[tid] || !inst) {
        return;
    }

    const cfi_type_t cfi_type = decoupledBPUCFIType(inst);
    if (cfi_type == CFI_NULL) {
        return;
    }

    const Addr inst_addr = inst->pcState().instAddr();
    const DecoupledBpuFetchPredictionSegment* segment =
        findDecoupledBPUFetchPrediction(tid, inst_addr);
    const Addr entry_base = segment ? segment->entryBase :
        fetchBufferAlignPC(inst_addr);
    if (inst_addr < entry_base) {
        return;
    }

    const int entry_offset_2b =
        static_cast<int>((inst_addr - entry_base) / 2);
    const int cfi_block_index =
        entry_offset_2b / bpu_cfg::fetch_block_2b_count;
    const Addr cfi_block_addr =
        entry_base +
        cfi_block_index * bpu_cfg::fetch_block_2b_count * 2;
    const int cfi_block_offset_2b =
        entry_offset_2b -
        cfi_block_index * bpu_cfg::fetch_block_2b_count;

    bpu_sign_t sign;
    sign.offset = cfi_block_offset_2b;
    sign.type = cfi_type;
    sign.compressed = inst->staticInst->size() <= 2;
    sign.is_call = inst->isCall();

    const bool taken_cfi =
        inst->isUncondCtrl() || inst->isIndirectCtrl() ||
        inst->readPredTaken();
    if (taken_cfi) {
        const Addr predicted_cfi_addr =
            segment && segment->takenValid ?
            segment->entryBase +
                static_cast<Addr>(segment->takenSign.offset) * 2 :
            0;
        const bool btb_marker_matches =
            segment && segment->takenValid &&
            predicted_cfi_addr == inst_addr &&
            decoupledBPUTypeMatches(inst, segment->takenSign.type);
        if (btb_marker_matches) {
            ++fetchStats.decoupledBpuBtbHits;
        } else {
            // If the decoded block has no CFI, no counter is touched. A miss
            // is only charged when a real taken/stopping CFI was fetched and
            // the FTQ did not carry the matching BTB marker for this PC.
            ++fetchStats.decoupledBpuBtbMisses;
        }
    }

    Addr target = 0;
    bool target_valid = false;
    if (inst->isDirectCtrl()) {
        std::unique_ptr<PCStateBase> direct_target = inst->branchTarget();
        target = direct_target->instAddr();
        target_valid = true;
    } else if (inst->readPredTaken()) {
        target = inst->readPredTarg().instAddr();
        target_valid = true;
    }
    cfi_tracer::cfi_point_t point;
    point.sign = sign;
    point.target = target_valid ? static_cast<bpu_addr_t>(target) : 0;
    point.next_cfi_span_2b = 0;
    point.seq_num = inst->seqNum;

    decoupledBPUTracers[tid]->add_fetch_block(
        tid, static_cast<bpu_addr_t>(cfi_block_addr), inst->seqNum,
        inst->seqNum,
        {point});

    std::vector<btb_commit_update_t> btb_updates =
        decoupledBPUTracers[tid]->make_btb_commit_updates(
            tid, static_cast<bpu_addr_t>(cfi_block_addr));

    for (btb_commit_update_t& btb_update : btb_updates) {
        bpu_commit_update_t update;
        update.btb_update = btb_update;
        update.speculative_id = segment ? segment->speculativeId : -1;
        update.btb_update.update_branch_ctr = cfi_type == CFI_BRA;
        update.btb_update.branch_sign = sign;
        update.btb_update.branch_taken =
            inst->isUncondCtrl() || inst->readPredTaken();

        auto tune_record = [cfi_type](btb_entry_record_t& record) {
            if (!record.valid) {
                return;
            }

            if (cfi_type == CFI_BRA) {
                record.taken_next_cfi_span_2b = 0;
                record.fallthrough_next_cfi_span_2b =
                    bpu_cfg::fetch_block_2b_count;
            } else {
                record.taken_next_cfi_span_2b = 0;
                record.fallthrough_next_cfi_span_2b = 0;
            }
        };
        tune_record(update.btb_update.entry.e1);
        tune_record(update.btb_update.entry.e2);

        if (cfi_type == CFI_JALR_CALL && target_valid &&
            inst->readPredTaken()) {
            update.tt_update.valid = true;
            update.tt_update.pc = update.btb_update.pc;
            update.tt_update.lookup_pc_valid =
                update.btb_update.lookup_pc_valid;
            update.tt_update.lookup_pc = update.btb_update.lookup_pc;
            update.tt_update.target = static_cast<bpu_addr_t>(target);
        }

        DecoupledBpuPendingCommit pending;
        pending.valid = true;
        pending.seqNum = inst->seqNum;
        pending.update = update;
        decoupledBPUPendingCommits[tid].push_back(pending);

        ++fetchStats.decoupledBpuCfiRecords;
    }
}

void
Fetch::updateDecoupledBPUMispredictCommitInfo(
    ThreadID tid, const DynInstPtr &inst, bool branch_taken,
    const PCStateBase &target)
{
    if (!decoupledBPUEnabled || !inst) {
        return;
    }

    const cfi_type_t cfi_type = decoupledBPUCFIType(inst);
    if (cfi_type == CFI_NULL) {
        return;
    }

    for (auto it = decoupledBPUPendingCommits[tid].rbegin();
         it != decoupledBPUPendingCommits[tid].rend(); ++it) {
        if (!it->valid || it->seqNum != inst->seqNum) {
            continue;
        }

        it->update.btb_update.branch_taken =
            branch_taken || inst->isUncondCtrl();
        it->update.btb_update.update_branch_ctr = cfi_type == CFI_BRA;

        if (inst->isIndirectCtrl()) {
            const bpu_addr_t actual_target =
                it->update.btb_update.branch_taken ?
                static_cast<bpu_addr_t>(target.instAddr()) : 0;
            auto patch_target =
                [actual_target](btb_entry_record_t& record) {
                    if (record.valid) {
                        record.target = actual_target;
                    }
                };
            patch_target(it->update.btb_update.entry.e1);
            patch_target(it->update.btb_update.entry.e2);

            if (cfi_type == CFI_JALR_CALL &&
                it->update.btb_update.branch_taken) {
                it->update.tt_update.valid = true;
                it->update.tt_update.pc = it->update.btb_update.pc;
                it->update.tt_update.lookup_pc_valid =
                    it->update.btb_update.lookup_pc_valid;
                it->update.tt_update.lookup_pc =
                    it->update.btb_update.lookup_pc;
                it->update.tt_update.target = actual_target;
            }
        }

        return;
    }
}

void
Fetch::commitDecoupledBPU(ThreadID tid, InstSeqNum done_seq,
                          bool retire_speculation)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid] || done_seq == 0) {
        return;
    }

    auto& pending = decoupledBPUPendingCommits[tid];
    while (!pending.empty() && pending.front().seqNum <= done_seq) {
        DecoupledBpuPendingCommit entry = pending.front();
        pending.pop_front();

        if (!entry.valid) {
            continue;
        }

        decoupledBpus[tid]->commit(entry.update);
        if (retire_speculation) {
            decoupledBpus[tid]->retire_speculative_through(
                entry.update.speculative_id);
        }
        if (entry.update.btb_update.valid &&
            entry.update.btb_update.insert_entry) {
            ++fetchStats.decoupledBpuBtbCommitInserts;
        }
        if (entry.update.btb_update.valid &&
            entry.update.btb_update.update_branch_ctr) {
            ++fetchStats.decoupledBpuBtbCtrUpdates;
        }
        if (entry.update.tt_update.valid) {
            ++fetchStats.decoupledBpuTTCommitUpdates;
        }

    }

    if (decoupledBPUTracers[tid]) {
        decoupledBPUTracers[tid]->commit_until(tid, done_seq);
    }

    if (decoupledBPUJalrStall[tid] &&
        decoupledBPUJalrStallSeqNum[tid] <= done_seq) {
        clearDecoupledBPUJalrStall(tid);
    }
}

void
Fetch::squashDecoupledBPUCommitInfo(ThreadID tid, InstSeqNum done_seq)
{
    if (!decoupledBPUEnabled) {
        return;
    }

    auto& pending = decoupledBPUPendingCommits[tid];
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
            [done_seq](const DecoupledBpuPendingCommit& entry) {
                return entry.seqNum > done_seq;
            }),
        pending.end());

    if (decoupledBPUTracers[tid]) {
        decoupledBPUTracers[tid]->squash_after(tid, done_seq);
    }
}

void
Fetch::tickDecoupledBPU(ThreadID tid)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid]) {
        return;
    }

    ++fetchStats.decoupledBpuTicks;
    if (decoupledBPUJalrStall[tid]) {
        return;
    }

    if (decoupledBpus[tid]->ftq_full()) {
        ++fetchStats.decoupledBpuFtqFullOnTick;
        if (!decoupledBPUFtqFullLastCycle[tid]) {
            DPRINTF(DecoupledBPU,
                    "[tid:%i] FTQ is full; BPU holds runahead until fetch "
                    "consumes entries\n",
                    tid);
        }
        decoupledBPUFtqFullLastCycle[tid] = true;
        return;
    }
    if (decoupledBPUFtqFullLastCycle[tid]) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] FTQ has space again; BPU resumes runahead\n", tid);
    }
    decoupledBPUFtqFullLastCycle[tid] = false;

    bpu_cycle_input_t input =
        makeDecoupledBPUInput(tid, pc[tid]->instAddr());
    const bpu_cycle_output_t output = decoupledBpus[tid]->tick(input);
    fetchStats.decoupledBpuSpeculativeNodesMax =
        std::max<double>(decoupledBpus[tid]->speculative_node_count(),
                         fetchStats.decoupledBpuSpeculativeNodesMax.value());
    fetchStats.decoupledBpuRASDepthMax =
        std::max<double>(decoupledBpus[tid]->ras_depth(),
                         fetchStats.decoupledBpuRASDepthMax.value());
    fetchStats.decoupledBpuTageCheckpointsMax =
        std::max<double>(decoupledBpus[tid]->tage_checkpoint_count(),
                         fetchStats.decoupledBpuTageCheckpointsMax.value());
    fetchStats.decoupledBpuITTAGECheckpointsMax =
        std::max<double>(
            decoupledBpus[tid]->ittage_checkpoint_count(),
            fetchStats.decoupledBpuITTAGECheckpointsMax.value());
    if (output.bpu2.speculative_id >= 0) {
        decoupledBPULastSpeculativeId[tid] = output.bpu2.speculative_id;
    }
    sampleDecoupledBPUOutput(tid, output);

    if (output.bpu1.valid && output.bpu1.taken) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] bpu1 predict type=%s cfi=%#x offset=%i "
                "target=%#x ftq_span=%i->%i add=%i next_cfi=%#x\n",
                tid, decoupledBPUCFITypeName(output.bpu1.sign.type),
                output.lookup_cfi_addr, output.bpu1.sign.offset,
                output.bpu1.target, output.bpu1_old_fetch_span_2b,
                output.bpu1_new_fetch_span_2b,
                output.bpu1_added_fetch_span_2b,
                output.bpu1_next_cfi_addr);
    } else {
        DPRINTF(DecoupledBPU,
                "[tid:%i] bpu1 extend cfi=%#x hit=%i ftq_span=%i->%i "
                "add=%i next_cfi=%#x\n",
                tid, output.lookup_cfi_addr, output.bpu1.valid,
                output.bpu1_old_fetch_span_2b,
                output.bpu1_new_fetch_span_2b,
                output.bpu1_added_fetch_span_2b,
                output.bpu1_next_cfi_addr);
    }

    if (output.bpu2.valid) {
        const bool is_indirect =
            output.bpu2.sign.type == CFI_JALR_CALL ||
            output.bpu2.sign.type == CFI_JALR_RET;
        const bool wait_backend = is_indirect && !output.bpu2.taken;
        const char* action = output.bpu2.taken ? "redirect" :
            wait_backend ? "wait_backend" : "fallthrough";
        DPRINTF(DecoupledBPU,
                "[tid:%i] bpu2 %s type=%s cfi=%#x offset=%i target=%#x "
                "ftq_span=%i->%i next_cfi=%#x spec=%i tage_used=%i "
                "tt_hit=%i ras_hit=%i\n",
                tid, action, decoupledBPUCFITypeName(output.bpu2.sign.type),
                output.lookup_cfi_addr, output.bpu2.sign.offset,
                output.bpu2.target, output.bpu2_old_fetch_span_2b,
                output.bpu2_new_fetch_span_2b, output.bpu2_next_cfi_addr,
                output.bpu2.speculative_id, output.bpu2.tage_used,
                output.bpu2.tt_hit, output.bpu2.ras_valid);
    }

    if (output.bpu3_redirect) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] bpu3 redirect type=%s cfi=%#x offset=%i "
                "target=%#x ftq_span=%i->%i next_cfi=%#x "
                "ittage_hit=%i spec=%i\n",
                tid, decoupledBPUCFITypeName(output.redirect.sign.type),
                output.lookup_cfi_addr, output.redirect.sign.offset,
                output.redirect.target, output.bpu3_old_fetch_span_2b,
                output.bpu3_new_fetch_span_2b, output.bpu3_next_cfi_addr,
                output.ittage_response.hit, output.bpu2.speculative_id);
    }
}

void
Fetch::pumpDecoupledBPU(ThreadID tid, unsigned budget)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid] || budget == 0) {
        return;
    }

    ++fetchStats.decoupledBpuBursts;

    for (unsigned i = 0; i < budget; ++i) {
        tickDecoupledBPU(tid);
        if (decoupledBPUJalrStall[tid] ||
            decoupledBpus[tid]->ftq_full()) {
            break;
        }
    }
}

bool
Fetch::decoupledBPUCanFetch(ThreadID tid, Addr fetch_addr)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid]) {
        return true;
    }

    if (decoupledBPUJalrStall[tid]) {
        return false;
    }

    bool stale_recovered_to_fetch = false;
    int stale_retire_budget =
        static_cast<int>(std::max(1U, decoupledBPUFTQDepth)) + 4;
    while (true) {
        if (!decoupledBpus[tid]->ftq_ready()) {
            ++fetchStats.decoupledBpuFtqEmptyOnRequest;
            pumpDecoupledBPU(tid, decoupledBPUBurstTicks);
            if (!decoupledBpus[tid]->ftq_ready()) {
                return stale_recovered_to_fetch;
            }
        }

        const ftq_entry_t* front = decoupledBpus[tid]->get_ftq_front();
        const int front_available_span = front ?
            std::max(0, front->fetch_span_2b - front->consumed_span_2b) : 0;
        const Addr front_end = front ?
            static_cast<Addr>(front->base_addr + front->fetch_span_2b * 2) :
            0;

        if (front == nullptr || front_end > fetch_addr) {
            break;
        }

        if (fetchOffset[tid] != 0) {
            return true;
        }

        if (stale_retire_budget-- <= 0) {
            DPRINTF(DecoupledBPU,
                    "[tid:%i] FTQ keeps producing stale coverage behind "
                    "fetch request %#x; recovering BPU to fetch PC\n",
                    tid, fetch_addr);
            decoupledBpus[tid]->recover(static_cast<bpu_addr_t>(fetch_addr),
                                        -1, true);
            decoupledBPULastSpeculativeId[tid] = -1;
            stale_recovered_to_fetch = true;
            pumpDecoupledBPU(tid, decoupledBPUBurstTicks);
            continue;
        }

        recordDecoupledBPUFetchPrediction(
            tid, *front, front->consumed_span_2b, front_available_span);
        decoupledBpus[tid]->consume_ftq(front_available_span);
        ++fetchStats.decoupledBpuFtqConsumes;

        if (front_available_span <= 0) {
            break;
        }
    }

    const ftq_entry_t* front = decoupledBpus[tid]->get_ftq_front();
    const Addr front_fetch_addr = front ? static_cast<Addr>(
        front->base_addr + front->consumed_span_2b * 2) : 0;
    const Addr front_end = front ?
        static_cast<Addr>(front->base_addr + front->fetch_span_2b * 2) : 0;
    if (front == nullptr || fetch_addr == front_fetch_addr ||
        (fetch_addr > front_fetch_addr && fetch_addr < front_end)) {
        return true;
    }

    if (fetchOffset[tid] != 0) {
        return true;
    }

    if (pc[tid]->instAddr() != fetch_addr) {
        return true;
    }

    DPRINTF(DecoupledBPU,
            "[tid:%i] FTQ front %#x does not cover fetch request %#x; "
            "recovering BPU to fetch PC\n",
            tid, front_fetch_addr, fetch_addr);
    decoupledBpus[tid]->recover(static_cast<bpu_addr_t>(fetch_addr), -1,
                                true);
    decoupledBPULastSpeculativeId[tid] = -1;
    pumpDecoupledBPU(tid, decoupledBPUBurstTicks);

    return true;
}

void
Fetch::recoverDecoupledBPU(ThreadID tid, const PCStateBase &new_pc,
                           const DynInstPtr &squashInst, bool include_self)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid]) {
        return;
    }

    const int speculative_id = squashInst ?
        squashInst->getDecoupledBpuSpeculativeId() : -1;
    decoupledBpus[tid]->recover(static_cast<bpu_addr_t>(new_pc.instAddr()),
                                speculative_id, include_self);
    decoupledBPULastSpeculativeId[tid] = -1;
    clearDecoupledBPUJalrStall(tid);

    DPRINTF(DecoupledBPU,
            "[tid:%i] recover pc=%#x spec=%i include_self=%i\n",
            tid, new_pc.instAddr(), speculative_id, include_self);
}

void
Fetch::clearDecoupledBPUFetchPredictions(ThreadID tid)
{
    decoupledBPUFetchPredictions[tid].clear();
}

void
Fetch::recordDecoupledBPUFetchPrediction(ThreadID tid,
                                         const ftq_entry_t &entry,
                                         int span_start_2b,
                                         int span_count_2b)
{
    if (!decoupledBPUEnabled || span_count_2b <= 0) {
        return;
    }

    DecoupledBpuFetchPredictionSegment segment;
    segment.valid = true;
    segment.entryBase = static_cast<Addr>(entry.base_addr);
    segment.spanStart2B = std::max(0, span_start_2b);
    segment.spanCount2B = std::max(0, span_count_2b);
    segment.takenSign = entry.cfi_taken_sign;
    segment.target = static_cast<Addr>(entry.target);
    segment.jalrFail = entry.jalr_fail;
    segment.speculativeId = entry.speculative_id;

    const int span_end_2b = segment.spanStart2B + segment.spanCount2B;
    segment.takenValid =
        segment.takenSign.type != CFI_NULL &&
        segment.takenSign.offset >= segment.spanStart2B &&
        segment.takenSign.offset < span_end_2b;

    decoupledBPUFetchPredictions[tid].push_back(segment);
    while (decoupledBPUFetchPredictions[tid].size() > 128) {
        decoupledBPUFetchPredictions[tid].erase(
            decoupledBPUFetchPredictions[tid].begin());
    }

}

const Fetch::DecoupledBpuFetchPredictionSegment*
Fetch::findDecoupledBPUFetchPrediction(ThreadID tid, Addr inst_addr) const
{
    const auto& predictions = decoupledBPUFetchPredictions[tid];
    for (auto it = predictions.rbegin(); it != predictions.rend(); ++it) {
        if (!it->valid || it->spanCount2B <= 0) {
            continue;
        }

        const Addr span_start =
            it->entryBase + static_cast<Addr>(it->spanStart2B) * 2;
        const Addr span_end =
            span_start + static_cast<Addr>(it->spanCount2B) * 2;
        if (inst_addr >= span_start && inst_addr < span_end) {
            return &*it;
        }
    }

    return nullptr;
}

void
Fetch::stallDecoupledBPUForJalr(ThreadID tid, const DynInstPtr &inst)
{
    if (!decoupledBPUEnabled || !inst || !inst->isIndirectCtrl()) {
        return;
    }

    if (!decoupledBPUJalrStall[tid]) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] [sn:%llu] stalling fetch after unresolved "
                "JALR/RET pc=%#x\n",
                tid, inst->seqNum, inst->pcState().instAddr());
    }

    decoupledBPUJalrStall[tid] = true;
    decoupledBPUJalrStallSeqNum[tid] = inst->seqNum;
    decoupledBPUJalrStallPC[tid] = inst->pcState().instAddr();
}

bool
Fetch::decoupledBPUWaitingForJalr(ThreadID tid,
                                  const DynInstPtr &inst) const
{
    return decoupledBPUEnabled && inst &&
        decoupledBPUJalrStall[tid] &&
        decoupledBPUJalrStallSeqNum[tid] == inst->seqNum;
}

void
Fetch::clearDecoupledBPUJalrStall(ThreadID tid)
{
    if (!decoupledBPUJalrStall[tid]) {
        return;
    }

    DPRINTF(DecoupledBPU,
            "[tid:%i] clearing JALR/RET fetch stall pc=%#x seq=%llu\n",
            tid, decoupledBPUJalrStallPC[tid],
            decoupledBPUJalrStallSeqNum[tid]);
    decoupledBPUJalrStall[tid] = false;
    decoupledBPUJalrStallSeqNum[tid] = 0;
    decoupledBPUJalrStallPC[tid] = 0;
}

bool
Fetch::applyDecoupledBPUPrediction(const DynInstPtr &inst,
                                   PCStateBase &next_pc,
                                   bool &predict_taken)
{
    if (!decoupledBPUEnabled || !decoupledBpus[inst->threadNumber]) {
        return false;
    }

    ThreadID tid = inst->threadNumber;
    std::unique_ptr<PCStateBase> fallthrough_pc(inst->pcState().clone());
    inst->staticInst->advancePC(*fallthrough_pc);
    predict_taken = false;

    const DecoupledBpuFetchPredictionSegment* segment =
        findDecoupledBPUFetchPrediction(tid, inst->pcState().instAddr());
    if (segment == nullptr) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] [sn:%llu] no FTQ prediction for pc=%#x; "
                "leaving prediction to legacy frontend %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
        return false;
    }

    if (!segment->takenValid) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] [sn:%llu] no taken FTQ prediction for pc=%#x; "
                "leaving direct prediction to legacy frontend %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
        if (inst->isIndirectCtrl()) {
            set(next_pc, *fallthrough_pc);
            stallDecoupledBPUForJalr(tid, inst);
            return true;
        }
        return false;
    }

    const Addr taken_addr =
        segment->entryBase + static_cast<Addr>(segment->takenSign.offset) * 2;
    const int window_offset_2b =
        segment->takenSign.offset - segment->spanStart2B;
    if (inst->pcState().instAddr() != taken_addr) {
        if (inst->isIndirectCtrl()) {
            set(next_pc, *fallthrough_pc);
            stallDecoupledBPUForJalr(tid, inst);
            return true;
        }
        return false;
    }

    if (segment->jalrFail ||
        !decoupledBPUTypeMatches(inst, segment->takenSign.type)) {
        DPRINTF(DecoupledBPU,
                "[tid:%i] [sn:%llu] FTQ CFI marker pc=%#x "
                "entry_offset=%i window_offset=%i type=%i jalr_fail=%i "
                "does not produce a fetch prediction\n",
                tid, inst->seqNum, inst->pcState().instAddr(),
                segment->takenSign.offset, window_offset_2b,
                segment->takenSign.type, segment->jalrFail);
        if (inst->isIndirectCtrl()) {
            set(next_pc, *fallthrough_pc);
            stallDecoupledBPUForJalr(tid, inst);
            return true;
        }
        return false;
    }

    next_pc.set(segment->target);
    predict_taken = true;

    DPRINTF(DecoupledBPU,
            "[tid:%i] [sn:%llu] FTQ predicted taken pc=%#x "
            "entry_base=%#x entry_offset=%i window_offset=%i target=%s "
            "spec=%i\n",
            tid, inst->seqNum, inst->pcState().instAddr(),
            segment->entryBase, segment->takenSign.offset, window_offset_2b,
            next_pc, segment->speculativeId);
    return true;
}

bool
Fetch::consumeDecoupledFTQCacheBlock(ThreadID tid, Addr fetch_addr)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid]) {
        return false;
    }

    const Addr block_start = fetchBufferAlignPC(fetch_addr);
    const Addr block_end = block_start + fetchBufferSize;
    bool consumed_any = false;

    while (true) {
        const ftq_entry_t* front = decoupledBpus[tid]->get_ftq_front();
        if (front == nullptr || front->fetch_span_2b <= 0) {
            return consumed_any;
        }

        const Addr entry_base = static_cast<Addr>(front->base_addr);
        const Addr front_base = static_cast<Addr>(
            front->base_addr + front->consumed_span_2b * 2);
        const int span_start_2b = front->consumed_span_2b;
        const int front_span_2b =
            std::max(0, front->fetch_span_2b - front->consumed_span_2b);
        const Addr front_end = entry_base + front->fetch_span_2b * 2;

        if (front_base >= block_end || front_end <= block_start) {
            return consumed_any;
        }

        const Addr consume_end = std::min(front_end, block_end);
        const int two_byte_count =
            static_cast<int>((consume_end - front_base) / 2);
        if (two_byte_count <= 0) {
            return consumed_any;
        }

        recordDecoupledBPUFetchPrediction(tid, *front, span_start_2b,
                                           two_byte_count);
        decoupledBpus[tid]->consume_ftq(two_byte_count);
        consumed_any = true;
        ++fetchStats.decoupledBpuFtqConsumes;

        if (two_byte_count < front_span_2b) {
            return true;
        }
    }
}

bool
Fetch::checkDecoupledBPUPredecode(ThreadID tid, const DynInstPtr &inst,
                                  PCStateBase &next_pc)
{
    if (!decoupledBPUEnabled || !decoupledBpus[tid] || !inst) {
        return false;
    }

    const DecoupledBpuFetchPredictionSegment* segment =
        findDecoupledBPUFetchPrediction(tid, inst->pcState().instAddr());
    if (segment != nullptr && segment->takenValid) {
        const Addr taken_addr =
            segment->entryBase +
            static_cast<Addr>(segment->takenSign.offset) * 2;
        if (inst->pcState().instAddr() == taken_addr &&
            !decoupledBPUTypeMatches(inst, segment->takenSign.type)) {
            if (inst->isIndirectCtrl()) {
                stallDecoupledBPUForJalr(tid, inst);
                return false;
            }

            DPRINTF(DecoupledBPU,
                    "[tid:%i] [sn:%llu] false FTQ CFI marker pc=%#x "
                    "pred_type=%i actual_control=%i; recover to %s\n",
                    tid, inst->seqNum, inst->pcState().instAddr(),
                    segment->takenSign.type, inst->isControl(), next_pc);
            recoverDecoupledBPU(tid, next_pc, inst, true);
            ++fetchStats.decoupledBpuFetchPredecodeRedirects;
            return true;
        }
    }

    if (!inst->isDirectCtrl() ||
        !(inst->isUncondCtrl() || inst->readPredTaken())) {
        return false;
    }

    std::unique_ptr<PCStateBase> target = inst->branchTarget();
    if (*target == inst->readPredTarg()) {
        return false;
    }

    DPRINTF(DecoupledBPU,
            "[tid:%i] [sn:%llu] predecode target correction pc=%#x "
            "pred=%s actual=%s\n",
            tid, inst->seqNum, inst->pcState().instAddr(),
            inst->readPredTarg(), *target);

    set(next_pc, *target);
    inst->setPredTarg(*target);
    inst->setPredTaken(true);
    recoverDecoupledBPU(tid, *target, inst, true);
    ++fetchStats.decoupledBpuFetchPredecodeRedirects;
    return true;
}

Fetch::DecoupledBpuRedirectType
Fetch::decoupledBPURedirectType(const DynInstPtr &inst) const
{
    if (!inst) {
        return DecoupledBpuOther;
    }

    if (inst->isReturn()) {
        return DecoupledBpuReturn;
    }

    if (inst->isIndirectCtrl()) {
        return DecoupledBpuIndirect;
    }

    if (inst->isDirectCtrl() && inst->isCondCtrl()) {
        return DecoupledBpuDirectCond;
    }

    if (inst->isDirectCtrl()) {
        return DecoupledBpuDirectUncond;
    }

    return DecoupledBpuOther;
}

void
Fetch::countDecoupledBPURedirect(const DynInstPtr &inst, bool from_decode)
{
    if (!decoupledBPUEnabled) {
        return;
    }

    if (from_decode) {
        ++fetchStats.decoupledBpuDecodeRedirects;
    } else {
        ++fetchStats.decoupledBpuBackendRedirects;
    }

    switch (decoupledBPURedirectType(inst)) {
      case DecoupledBpuDirectCond:
        if (from_decode) {
            ++fetchStats.decoupledBpuDecodeDirectCondRedirects;
        } else {
            ++fetchStats.decoupledBpuBackendDirectCondRedirects;
        }
        break;
      case DecoupledBpuDirectUncond:
        if (from_decode) {
            ++fetchStats.decoupledBpuDecodeDirectUncondRedirects;
        } else {
            ++fetchStats.decoupledBpuBackendDirectUncondRedirects;
        }
        break;
      case DecoupledBpuIndirect:
        if (from_decode) {
            ++fetchStats.decoupledBpuDecodeIndirectRedirects;
        } else {
            ++fetchStats.decoupledBpuBackendIndirectRedirects;
        }
        break;
      case DecoupledBpuReturn:
        if (from_decode) {
            ++fetchStats.decoupledBpuDecodeReturnRedirects;
        } else {
            ++fetchStats.decoupledBpuBackendReturnRedirects;
        }
        break;
      case DecoupledBpuOther:
        if (from_decode) {
            ++fetchStats.decoupledBpuDecodeOtherRedirects;
        } else {
            ++fetchStats.decoupledBpuBackendOtherRedirects;
        }
        break;
    }
}

void
Fetch::squash(const PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, tid);

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    bool status_change = false;

    wroteToTimeBuffer = false;

    for (ThreadID i = 0; i < numThreads; ++i) {
        issuePipelinedIfetch[i] = false;
    }

    for (ThreadID tid : *activeThreads) {
        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            interruptPending = false;
        }
    }

    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Issue the next I-cache request if possible.
    for (ThreadID i = 0; i < numThreads; ++i) {
        if (issuePipelinedIfetch[i]) {
            pipelineIcacheAccesses(i);
        }
    }

    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            rng->random<uint8_t>(0, activeThreads->size() - 1));

    while (available_insts != 0 && insts_to_decode < decodeWidth) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());

            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

    // If there was activity this cycle, inform the CPU of it.
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of the instruction we've fetched.
    numInst = 0;
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    // Update the per thread stall statuses.
    if (fromDecode->decodeBlock[tid]) {
        stalls[tid].decode = true;
    }

    if (fromDecode->decodeUnblock[tid]) {
        assert(stalls[tid].decode);
        assert(!fromDecode->decodeBlock[tid]);
        stalls[tid].decode = false;
    }

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",tid);
        // In any case, squash.
        squash(*fromCommit->commitInfo[tid].pc,
               fromCommit->commitInfo[tid].doneSeqNum,
               fromCommit->commitInfo[tid].squashInst, tid);

        // If it was a branch mispredict on a control instruction, update the
        // branch predictor with that instruction, otherwise just kill the
        // invalid state we generated in after sequence number
        if (fromCommit->commitInfo[tid].mispredictInst &&
            fromCommit->commitInfo[tid].mispredictInst->isControl()) {
            updateDecoupledBPUMispredictCommitInfo(
                tid, fromCommit->commitInfo[tid].mispredictInst,
                fromCommit->commitInfo[tid].branchTaken,
                *fromCommit->commitInfo[tid].pc);
            commitDecoupledBPU(tid, fromCommit->commitInfo[tid].doneSeqNum,
                               false);
            branchPred->squash(fromCommit->commitInfo[tid].doneSeqNum,
                    *fromCommit->commitInfo[tid].pc,
                    fromCommit->commitInfo[tid].branchTaken, tid);
        } else {
            branchPred->squash(fromCommit->commitInfo[tid].doneSeqNum,
                              tid);
        }
        squashDecoupledBPUCommitInfo(tid,
                                     fromCommit->commitInfo[tid].doneSeqNum);

        const DynInstPtr redirect_inst =
            fromCommit->commitInfo[tid].mispredictInst ?
            fromCommit->commitInfo[tid].mispredictInst :
            fromCommit->commitInfo[tid].squashInst;
        countDecoupledBPURedirect(redirect_inst, false);
        recoverDecoupledBPU(tid, *fromCommit->commitInfo[tid].pc,
                            redirect_inst, true);

        return true;
    } else if (fromCommit->commitInfo[tid].doneSeqNum) {
        // Update the branch predictor if it wasn't a squashed instruction
        // that was broadcasted.
        branchPred->update(fromCommit->commitInfo[tid].doneSeqNum, tid);
        commitDecoupledBPU(tid, fromCommit->commitInfo[tid].doneSeqNum);
    }

    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        // Update the branch predictor.
        if (fromDecode->decodeInfo[tid].branchMispredict) {
            updateDecoupledBPUMispredictCommitInfo(
                tid, fromDecode->decodeInfo[tid].mispredictInst,
                fromDecode->decodeInfo[tid].branchTaken,
                *fromDecode->decodeInfo[tid].nextPC);
            commitDecoupledBPU(tid, fromDecode->decodeInfo[tid].doneSeqNum,
                               false);
            branchPred->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                    *fromDecode->decodeInfo[tid].nextPC,
                    fromDecode->decodeInfo[tid].branchTaken, tid);
        } else {
            branchPred->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                              tid);
        }
        squashDecoupledBPUCommitInfo(tid,
                                     fromDecode->decodeInfo[tid].doneSeqNum);

        countDecoupledBPURedirect(fromDecode->decodeInfo[tid].squashInst,
                                  true);
        recoverDecoupledBPU(tid, *fromDecode->decodeInfo[tid].nextPC,
                            fromDecode->decodeInfo[tid].squashInst, true);

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                *fromDecode->decodeInfo[tid].nextPC);
            // Squash unless we're already squashing
            squashFromDecode(*fromDecode->decodeInfo[tid].nextPC,
                             fromDecode->decodeInfo[tid].squashInst,
                             fromDecode->decodeInfo[tid].doneSeqNum,
                             tid);

            return true;
        }
    }

    if (checkStall(tid) &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != QuiescePending) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        fetchStatus[tid] = Blocked;

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        fetchStatus[tid] = Running;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);
    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);
    if (decoupledBPUEnabled && decoupledBpus[tid]) {
        const DecoupledBpuFetchPredictionSegment* segment =
            findDecoupledBPUFetchPrediction(tid, this_pc.instAddr());
        const int speculative_id = segment ? segment->speculativeId : -1;
        instruction->setDecoupledBpuSpeculativeId(speculative_id);
    }

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    return instruction;
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {  // @todo Per-thread stats
            profileStall(0);
        }

        return;
    }

    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC.
    PCStateBase &this_pc = *pc[tid];

    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (this_pc.instAddr() + pcOffset) & decoder[tid]->pcMask();

    bool inRom = isRomMicroPC(this_pc.microPC());

    // If returning from the delay of a cache miss, then update the status
    // to running, otherwise do the cache access.  Possibly move this up
    // to tick() function.
    if (fetchStatus[tid] == IcacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);

        fetchStatus[tid] = Running;
        status_change = true;
    }

    if (decoupledBPUEnabled && decoupledBPUJalrStall[tid]) {
        decoupledBPUCanFetch(tid, fetchAddr);
        ++fetchStats.idleCycles;
        return;
    }

    if (fetchStatus[tid] == Running) {
        // Align the fetch PC so its at the start of a fetch buffer segment.
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        // If buffer is no longer valid or fetchAddr has moved to point
        // to the next cache block, AND we have no remaining ucode
        // from a macro-op, then start fetch from icache.
        if (!(fetchBufferValid[tid] &&
                    fetchBufferBlockPC == fetchBufferPC[tid]) && !inRom &&
                !macroop[tid]) {
            if (!decoupledBPUCanFetch(tid, fetchAddr)) {
                ++fetchStats.idleCycles;
                return;
            }

            pcOffset = fetchOffset[tid];
            fetchAddr = (this_pc.instAddr() + pcOffset) &
                decoder[tid]->pcMask();
            fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

            DPRINTF(Fetch, "[tid:%i] Attempting to translate and read "
                    "instruction, starting at PC %s.\n", tid, this_pc);

            const bool fetch_started =
                fetchCacheLine(fetchAddr, tid, this_pc.instAddr());
            if (fetch_started) {
                if (consumeDecoupledFTQCacheBlock(tid, fetchAddr)) {
                    pumpDecoupledBPU(tid, decoupledBPUBurstTicks);
                }
            }

            if (fetchStatus[tid] == IcacheWaitResponse) {
                cpu->fetchStats[tid]->icacheStallCycles++;
            }
            else if (fetchStatus[tid] == ItlbWait)
                ++fetchStats.tlbCycles;
            else
                ++fetchStats.miscStallCycles;
            return;
        } else if (fetchBufferValid[tid] &&
                   fetchBufferBlockPC == fetchBufferPC[tid] && !inRom &&
                   !macroop[tid]) {
            if (consumeDecoupledFTQCacheBlock(tid, fetchAddr)) {
                pumpDecoupledBPU(tid, decoupledBPUBurstTicks);
            }
        } else if (checkInterrupt(this_pc.instAddr()) &&
                !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted and we're not issuing
            // an delayed commit micro-op currently (delayed commit
            // instructions are not interruptable by interrupts, only faults)
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return;
        }
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }

        // Status is Idle, so fetch should do nothing.
        return;
    }

    ++fetchStats.cycles;

    std::unique_ptr<PCStateBase> next_pc(this_pc.clone());

    StaticInstPtr staticInst = NULL;
    StaticInstPtr curMacroop = macroop[tid];

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);

    // Need to keep track of whether or not a predicted branch
    // ended this fetch block.
    bool predictedBranch = false;

    // Need to halt fetch if quiesce instruction detected
    bool quiesce = false;

    const unsigned numInsts = fetchBufferSize / instSize;
    unsigned blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;

    auto *dec_ptr = decoder[tid];
    const Addr pc_mask = dec_ptr->pcMask();

    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize
           && !predictedBranch && !quiesce) {
        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        // in the decoder.
        bool needMem = !inRom && !curMacroop && !dec_ptr->instReady();
        fetchAddr = (this_pc.instAddr() + pcOffset) & pc_mask;
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        if (needMem) {
            // If buffer is no longer valid or fetchAddr has moved to point
            // to the next cache block then start fetch from icache.
            if (!fetchBufferValid[tid] ||
                fetchBufferBlockPC != fetchBufferPC[tid])
                break;

            if (blkOffset >= numInsts) {
                // We need to process more memory, but we've run out of the
                // current block.
                break;
            }

            memcpy(dec_ptr->moreBytesPtr(),
                    fetchBuffer[tid] + blkOffset * instSize, instSize);
            decoder[tid]->moreBytes(this_pc, fetchAddr);

            if (dec_ptr->needMoreBytes()) {
                blkOffset++;
                fetchAddr += instSize;
                pcOffset += instSize;
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            if (!(curMacroop || inRom)) {
                if (dec_ptr->instReady()) {
                    staticInst = dec_ptr->decode(this_pc);

                    // Increment stat of fetched instructions.
                    cpu->fetchStats[tid]->numInsts++;

                    if (staticInst->isMacroop()) {
                        curMacroop = staticInst;
                    } else {
                        pcOffset = 0;
                    }
                } else {
                    // We need more bytes for this instruction so blkOffset and
                    // pcOffset will be updated
                    break;
                }
            }
            // Whether we're moving to a new macroop because we're at the
            // end of the current one, or the branch predictor incorrectly
            // thinks we are...
            bool newMacro = false;
            if (curMacroop || inRom) {
                if (inRom) {
                    staticInst = dec_ptr->fetchRomMicroop(
                            this_pc.microPC(), curMacroop);
                } else {
                    staticInst = curMacroop->fetchMicroop(this_pc.microPC());
                }
                newMacro |= staticInst->isLastMicroop();
            }

            DynInstPtr instruction = buildInst(
                    tid, staticInst, curMacroop, this_pc, *next_pc, true);

            ppFetch->notify(instruction);
            numInst++;

#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
            }
#endif

            set(next_pc, this_pc);

            // If we're branching after this instruction, quit fetching
            // from the same block.
            predictedBranch |= this_pc.branching();
            predictedBranch |= lookupAndUpdateNextPC(instruction, *next_pc);
            const bool decoupled_jalr_wait =
                decoupledBPUWaitingForJalr(tid, instruction);
            predictedBranch |= decoupled_jalr_wait;
            if (predictedBranch) {
                DPRINTF(Fetch, "Branch detected with PC = %s\n", this_pc);
            }
            recordDecoupledBPUCommitInfo(tid, instruction);
            const bool predecode_corrected =
                checkDecoupledBPUPredecode(tid, instruction, *next_pc);
            predictedBranch |= predecode_corrected;
            if (decoupledBPUEnabled && predictedBranch &&
                !predecode_corrected && !decoupled_jalr_wait) {
                recoverDecoupledBPU(tid, *next_pc, instruction, true);
            }

            newMacro |= this_pc.instAddr() != next_pc->instAddr();

            // Move to the next instruction, unless we have a branch.
            set(this_pc, *next_pc);
            inRom = isRomMicroPC(this_pc.microPC());

            if (newMacro) {
                fetchAddr = this_pc.instAddr() & pc_mask;
                blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
                pcOffset = 0;
                curMacroop = NULL;
            }

            if (instruction->isQuiesce()) {
                DPRINTF(Fetch,
                        "Quiesce instruction encountered, halting fetch!\n");
                fetchStatus[tid] = QuiescePending;
                status_change = true;
                quiesce = true;
                break;
            }
        } while (!predictedBranch &&
                 (curMacroop || dec_ptr->instReady()) &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);

        // Re-evaluate whether the next instruction to fetch is in micro-op ROM
        // or not.
        inRom = isRomMicroPC(this_pc.microPC());
    }

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch "
                "instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth "
                "for this cycle.\n", tid);
    } else if (blkOffset >= fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached the end of the"
                "fetch buffer.\n", tid);
    }

    macroop[tid] = curMacroop;
    fetchOffset[tid] = pcOffset;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    // pipeline a fetch if we're crossing a fetch buffer boundary and not in
    // a state that would preclude fetching
    fetchAddr = (this_pc.instAddr() + pcOffset) & pc_mask;
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    issuePipelinedIfetch[tid] = fetchBufferBlockPC != fetchBufferPC[tid] &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != QuiescePending &&
        !curMacroop;
}

void
Fetch::recvReqRetry()
{
    if (retryPkt != NULL) {
        assert(cacheBlocked);
        assert(retryTid != InvalidThreadID);
        assert(fetchStatus[retryTid] == IcacheWaitRetry);

        if (icachePort.sendTimingReq(retryPkt)) {
            fetchStatus[retryTid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify(retryPkt->req);
            retryPkt = NULL;
            retryTid = InvalidThreadID;
            cacheBlocked = false;
        }
    } else {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
    }
}

///////////////////////////////////////
//                                   //
//  SMT FETCH POLICY MAINTAINED HERE //
//                                   //
///////////////////////////////////////
ThreadID
Fetch::getFetchingThread()
{
    if (numThreads > 1) {
        switch (fetchPolicy) {
          case SMTFetchPolicy::RoundRobin:
            return roundRobin();
          case SMTFetchPolicy::IQCount:
            return iqCount();
          case SMTFetchPolicy::LSQCount:
            return lsqCount();
          case SMTFetchPolicy::Branch:
            return branchCount();
          default:
            return InvalidThreadID;
        }
    } else {
        auto thread = activeThreads->begin();
        if (thread == activeThreads->end()) {
            return InvalidThreadID;
        }

        ThreadID tid = *thread;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == IcacheAccessComplete ||
            fetchStatus[tid] == Idle) {
            return tid;
        } else {
            return InvalidThreadID;
        }
    }
}


ThreadID
Fetch::roundRobin()
{
    auto pri_iter = priorityList.begin();
    auto end      = priorityList.end();

    ThreadID high_pri;

    while (pri_iter != end) {
        high_pri = *pri_iter;

        assert(high_pri <= numThreads);

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle) {

            priorityList.erase(pri_iter);
            priorityList.push_back(high_pri);

            return high_pri;
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
Fetch::iqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    for (ThreadID tid : *activeThreads) {
        unsigned iqCount = fromIEW->iewInfo[tid].iqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(iqCount);
        threadMap[iqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();

    }

    return InvalidThreadID;
}

ThreadID
Fetch::lsqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    for (ThreadID tid : *activeThreads) {
        unsigned ldstqCount = fromIEW->iewInfo[tid].ldstqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(ldstqCount);
        threadMap[ldstqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
Fetch::branchCount()
{
    panic("Branch Count Fetch policy unimplemented\n");
    return InvalidThreadID;
}

void
Fetch::pipelineIcacheAccesses(ThreadID tid)
{
    if (!issuePipelinedIfetch[tid]) {
        return;
    }

    // The next PC to access.
    PCStateBase &this_pc = *pc[tid];

    if (isRomMicroPC(this_pc.microPC())) {
        return;
    }

    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (this_pc.instAddr() + pcOffset) & decoder[tid]->pcMask();

    // Align the fetch PC so its at the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

    // Unless buffer already got the block, fetch it from icache.
    if (!(fetchBufferValid[tid] && fetchBufferBlockPC == fetchBufferPC[tid])) {
        if (!decoupledBPUCanFetch(tid, fetchAddr)) {
            return;
        }

        pcOffset = fetchOffset[tid];
        fetchAddr = (this_pc.instAddr() + pcOffset) &
            decoder[tid]->pcMask();
        fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
                "starting at PC %s.\n", tid, this_pc);

        const bool fetch_started =
            fetchCacheLine(fetchAddr, tid, this_pc.instAddr());
        if (fetch_started) {
            if (consumeDecoupledFTQCacheBlock(tid, fetchAddr)) {
                pumpDecoupledBPU(tid, decoupledBPUBurstTicks);
            }
        }
    }
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (stalls[tid].drain) {
        ++fetchStats.pendingDrainCycles;
        DPRINTF(Fetch, "Fetch is waiting for a drain!\n");
    } else if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitResponse) {
        cpu->fetchStats[tid]->icacheStallCycles++;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (fetchStatus[tid] == ItlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (fetchStatus[tid] == QuiescePending) {
        ++fetchStats.pendingQuiesceStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending quiesce "
                "instruction!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (fetchStatus[tid] == NoGoodAddr) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));
    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

} // namespace o3
} // namespace gem5
