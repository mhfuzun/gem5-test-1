/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "cpu/pred/my_tage.hh"

#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <string>

#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "debug/Branch.hh"

namespace gem5
{

namespace branch_prediction
{

namespace
{

template<typename T>
static void
checkVectorSize(const std::vector<T>& vec, unsigned expected, const char* name)
{
    if (vec.size() != expected) {
        fatal("MyTAGE config error: '%s' size (%u) must equal compCount (%u)",
              name, static_cast<unsigned>(vec.size()), expected);
    }
}

static history_hash_type_t
parseHistoryHashType(const std::string& value)
{
    if (value == "FOLDED" || value == "folded")
        return HISTORY_HASH_FOLDED;
    if (value == "CSR" || value == "CIRCULAR_SHIFT_REGISTER" ||
        value == "circular_shift_register")
        return HISTORY_HASH_CSR;
    fatal("MyTAGE config error: historyHashType must be FOLDED or CSR");
}

static replacement_mode_t
parseReplacementMode(const std::string& value)
{
    if (value == "USEFUL" || value == "USEFULL" || value == "useful")
        return REPLACEMENT_USEFUL;
    if (value == "PLRU" || value == "plru")
        return REPLACEMENT_PLRU;
    fatal("MyTAGE config error: replacementMode must be USEFUL or PLRU");
}

}  // namespace

tage_cfg_t
MyTAGE::buildCfg(const MyTAGEParams& params)
{
    const unsigned comp_count = params.compCount;
    if (params.fetchLineBytes == 0 ||
        (params.fetchLineBytes & (params.fetchLineBytes - 1)) != 0) {
        fatal(
            "MyTAGE config error: fetchLineBytes must be a non-zero power of "
            "two");
    }
    if (params.entryPerSet == 0) {
        fatal("MyTAGE config error: entryPerSet must be > 0");
    }
    if (params.bimodalCtrsPerRow == 0) {
        fatal("MyTAGE config error: bimodalCtrsPerRow must be > 0");
    }
    if (params.bimodalOffsetShift >= 64) {
        fatal("MyTAGE config error: bimodalOffsetShift must be < 64");
    }

    checkVectorSize(params.tableDepth, comp_count, "tableDepth");
    checkVectorSize(params.tableUsefulWidth, comp_count, "tableUsefulWidth");
    checkVectorSize(params.tableCtrWidth, comp_count, "tableCtrWidth");
    checkVectorSize(params.tableTagWidth, comp_count, "tableTagWidth");
    checkVectorSize(params.tableHistoryWidth, comp_count, "tableHistoryWidth");
    checkVectorSize(params.tablePcHistoryStart, comp_count,
                    "tablePcHistoryStart");
    checkVectorSize(params.tablePcHistoryWidth, comp_count,
                    "tablePcHistoryWidth");

    tage_cfg_t cfg{};
    cfg.bimodal_depth = params.bimodalDepth;
    cfg.ghistory_length = params.ghistoryLength;
    cfg.pchistory_length = params.pchistoryLength;
    cfg.comp_count = comp_count;
    cfg.entry_per_set = params.entryPerSet;
    cfg.fetch_line_bytes = params.fetchLineBytes;
    cfg.bimodal_ctrs_per_row = params.bimodalCtrsPerRow;
    cfg.bimodal_offset_shift = params.bimodalOffsetShift;
    cfg.bimodal_use_aligned_addr = params.bimodalUseAlignedAddr;
    cfg.tag_use_aligned_addr = params.tagUseAlignedAddr;

    cfg.pc_hash_start_for_idx = params.pcHashStartForIdx;
    cfg.pc_hash_width_for_idx = params.pcHashWidthForIdx;
    cfg.pc_hash_start_for_tag = params.pcHashStartForTag;
    cfg.pc_hash_width_for_tag = params.pcHashWidthForTag;

    cfg.allocate_randomplacement = params.allocateRandomPlacement;
    cfg.allocate_longerThanProvider = params.allocateLongerThanProvider;
    cfg.periodicreset = params.periodicReset;
    cfg.periodicreset_branchperiod = params.periodicResetBranchPeriod;

    cfg.random_type = params.useLfsr ? LFSR : SIMRAND;
    cfg.lfsr_width = params.lfsrWidth;
    cfg.lfsr_seed = params.lfsrSeed;
    cfg.lfsr_misprediction_update = params.lfsrMispredictionUpdate;
    cfg.history_hash_type = parseHistoryHashType(params.historyHashType);
    cfg.replacement_mode = parseReplacementMode(params.replacementMode);
    cfg.useAltOnNA = params.useAltOnNA;
    cfg.numUseAltOnNa = params.numUseAltOnNa;
    cfg.useAltOnNaBits = params.useAltOnNaBits;
    cfg.useAltOnNaHashStart = params.useAltOnNaHashStart;
    cfg.useAltOnNaHashWidth = params.useAltOnNaHashWidth;

    cfg.table_cfg.resize(comp_count);
    for (unsigned i = 0; i < comp_count; ++i) {
        table_cfg_t& row = cfg.table_cfg[i];
        row.depth = params.tableDepth[i];
        row.usefull_width = params.tableUsefulWidth[i];
        row.ctr_width = params.tableCtrWidth[i];
        row.tag_width = params.tableTagWidth[i];
        row.history_width = params.tableHistoryWidth[i];
        row.pchistory_start = params.tablePcHistoryStart[i];
        row.pchistory_width = params.tablePcHistoryWidth[i];
    }

    return cfg;
}

MyTAGE::MyTAGE(const MyTAGEParams& params)
    : BPredUnit(params), compCount(params.compCount),
      fetchLineBytes(params.fetchLineBytes), myStats(*this, params.compCount)
{
    tage_cfg_t cfg = buildCfg(params);

    // Keep deterministic behavior for SIMRAND mode via a configurable seed.
    if (cfg.random_type == SIMRAND) {
        std::srand(static_cast<unsigned>(cfg.lfsr_seed));
    }

    backendByThread.reserve(numThreads);
    lineHistoryByThread.resize(numThreads);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        try {
            backendByThread.push_back(std::make_unique<::tage>(cfg));
        } catch (const std::exception& e) {
            fatal("MyTAGE backend creation failed for tid %u: %s",
                  static_cast<unsigned>(tid), e.what());
        }
    }
}

void
MyTAGE::resetStats()
{
    statistics::Group::resetStats();
    for (const auto& backend : backendByThread) {
        backend->resetStats();
    }
}

statistics_t
MyTAGE::aggregateBackendStats() const
{
    statistics_t aggregate{};
    aggregate.tableUse.providerMapCount.assign(compCount, 0);
    aggregate.tableUse.altPredMapCount.assign(compCount, 0);
    aggregate.tableUse.allocationMapCount.assign(compCount, 0);
    aggregate.tableUse.providerHitCountList.assign(compCount, 0);
    aggregate.tableUse.altPredHitCountList.assign(compCount, 0);

    auto accumulateVector = [](std::vector<std::uint64_t>& dst,
                               const std::vector<std::uint64_t>& src) {
        assert(dst.size() == src.size());
        for (size_t i = 0; i < dst.size(); ++i) {
            dst[i] += src[i];
        }
    };

    for (const auto& backend : backendByThread) {
        const auto& stats = backend->getStats();
        aggregate.predictionLookupCount += stats.predictionLookupCount;
        aggregate.alignedPredictionLookupCount +=
            stats.alignedPredictionLookupCount;
        aggregate.offsetMatchCount += stats.offsetMatchCount;
        aggregate.tagMatchCount += stats.tagMatchCount;
        aggregate.providerFoundCount += stats.providerFoundCount;
        aggregate.altpredFoundCount += stats.altpredFoundCount;
        aggregate.bimodalFallbackNoOffsetCount +=
            stats.bimodalFallbackNoOffsetCount;
        aggregate.bimodalFallbackNoProviderCount +=
            stats.bimodalFallbackNoProviderCount;
        aggregate.providerPseudoNewAllocCount +=
            stats.providerPseudoNewAllocCount;
        aggregate.totalBranchCount += stats.totalBranchCount;
        aggregate.takenBranchCount += stats.takenBranchCount;
        aggregate.missPredictionCount += stats.missPredictionCount;
        aggregate.noAllocationBecauseOfHit += stats.noAllocationBecauseOfHit;
        aggregate.noAllocationBecauseOfnoFree +=
            stats.noAllocationBecauseOfnoFree;
        aggregate.noAllocationBecauseOfproviderHigh +=
            stats.noAllocationBecauseOfproviderHigh;
        aggregate.noAllocationButProviderZero +=
            stats.noAllocationButProviderZero;

        aggregate.tableUse.bimMapCount += stats.tableUse.bimMapCount;
        aggregate.tableUse.bimHitCount += stats.tableUse.bimHitCount;
        aggregate.tableUse.bimHitCountWhenBimUsed +=
            stats.tableUse.bimHitCountWhenBimUsed;
        aggregate.tableUse.altpredHitCount += stats.tableUse.altpredHitCount;
        aggregate.tableUse.altpredHitCountWhenAltpredUsed +=
            stats.tableUse.altpredHitCountWhenAltpredUsed;
        aggregate.tableUse.providerHitCount += stats.tableUse.providerHitCount;
        aggregate.tableUse.providerHitCountWhenProviderUsed +=
            stats.tableUse.providerHitCountWhenProviderUsed;

        accumulateVector(aggregate.tableUse.providerMapCount,
                         stats.tableUse.providerMapCount);
        accumulateVector(aggregate.tableUse.providerHitCountList,
                         stats.tableUse.providerHitCountList);
        accumulateVector(aggregate.tableUse.altPredMapCount,
                         stats.tableUse.altPredMapCount);
        accumulateVector(aggregate.tableUse.altPredHitCountList,
                         stats.tableUse.altPredHitCountList);
        accumulateVector(aggregate.tableUse.allocationMapCount,
                         stats.tableUse.allocationMapCount);
    }

    return aggregate;
}

Addr
MyTAGE::alignFetchLine(Addr pc) const
{
    const Addr line_bytes = fetchLineBytes ? fetchLineBytes : 1;
    return pc & ~(line_bytes - 1);
}

void
MyTAGE::startLineContext(ThreadID tid, Addr aligned_pc)
{
    auto& ctx = lineHistoryByThread[tid];
    if (ctx.valid && ctx.alignedPC == aligned_pc) {
        myStats.lineContextsReused++;
        return;
    }

    backendByThread[tid]->snapshotState(ctx.lookupBaseState);
    ctx.valid = true;
    ctx.alignedPC = aligned_pc;
    myStats.lineContextsStarted++;
}

void
MyTAGE::invalidateLineContext(ThreadID tid)
{
    auto& ctx = lineHistoryByThread[tid];
    if (ctx.valid) {
        ctx.valid = false;
        myStats.lineContextsInvalidated++;
    }
}

bool
MyTAGE::lookup(ThreadID tid, Addr pc, void*& bp_history)
{
    assert(tid < backendByThread.size());

    auto* hist = new MyTAGEHistory();
    hist->isConditional = true;
    backendByThread[tid]->snapshotState(hist->historyState);
    myStats.historySnapshotsCreated++;

    const Addr aligned_pc = alignFetchLine(pc);
    startLineContext(tid, aligned_pc);
    hist->predTaken = backendByThread[tid]->getPrediction(
        aligned_pc, pc, lineHistoryByThread[tid].lookupBaseState,
        hist->lookupState);
    myStats.lookupSnapshotsCreated++;
    bp_history = hist;

    return hist->predTaken;
}

void
MyTAGE::updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                        Addr target, const StaticInstPtr& inst,
                        void*& bp_history)
{
    assert(tid < backendByThread.size());
    (void)target;
    (void)inst;

    if (bp_history == nullptr) {
        auto* hist = new MyTAGEHistory();
        hist->isConditional = !uncond;
        hist->predTaken = uncond ? true : taken;
        backendByThread[tid]->snapshotState(hist->historyState);
        myStats.historySnapshotsCreated++;
        if (!uncond) {
            const Addr aligned_pc = alignFetchLine(pc);
            startLineContext(tid, aligned_pc);
            backendByThread[tid]->getPrediction(
                aligned_pc, pc, lineHistoryByThread[tid].lookupBaseState,
                hist->lookupState);
            myStats.lookupSnapshotsCreated++;
        }
        bp_history = hist;
    }

    auto* hist = static_cast<MyTAGEHistory*>(bp_history);
    backendByThread[tid]->updateHist(pc, taken, hist->historyState);
    myStats.speculativeHistoryUpdates++;

    if (taken) {
        invalidateLineContext(tid);
    }
}

void
MyTAGE::update(ThreadID tid, Addr pc, bool taken, void*& bp_history,
               bool squashed, const StaticInstPtr& inst, Addr target)
{
    (void)inst;
    (void)target;

    assert(tid < backendByThread.size());
    assert(bp_history);

    auto* hist = static_cast<MyTAGEHistory*>(bp_history);

    if (squashed) {
        backendByThread[tid]->resetState(hist->historyState);
        backendByThread[tid]->updateHist(pc, taken, hist->historyState);
        invalidateLineContext(tid);
        myStats.mispredictHistoryRepairs++;
        return;
    }

    if (hist->isConditional) {
        backendByThread[tid]->update(pc, taken, hist->lookupState);
        myStats.commitTableUpdates++;
    }

    delete hist;
    bp_history = nullptr;
}

void
MyTAGE::squash(ThreadID tid, void*& bp_history)
{
    assert(tid < backendByThread.size());

    auto* hist = static_cast<MyTAGEHistory*>(bp_history);
    backendByThread[tid]->resetState(hist->historyState);
    invalidateLineContext(tid);
    myStats.squashHistoryRestores++;
    delete hist;
    bp_history = nullptr;
}

void
MyTAGE::branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                          void*& bp_history)
{
    assert(tid < backendByThread.size());

    auto* hist = new MyTAGEHistory();
    hist->isConditional = !uncond;
    backendByThread[tid]->snapshotState(hist->historyState);
    myStats.historySnapshotsCreated++;
    hist->predTaken = uncond;
    bp_history = hist;
    myStats.placeholdersCreated++;

    DPRINTF(Branch,
            "MyTAGE placeholder created [tid:%u] PC:%#x cond:%d pred:%d\n",
            static_cast<unsigned>(tid), pc, !uncond, hist->predTaken);
}

MyTAGE::MyTAGEStats::MyTAGEStats(MyTAGE& parent, unsigned comp_count)
    : statistics::Group(&parent, "myTageStats"), myTage(parent),
      compCount(comp_count),
      ADD_STAT(totalBranches, statistics::units::Count::get(),
               "Total number of committed branches seen by MyTAGE backend"),
      ADD_STAT(takenBranches, statistics::units::Count::get(),
               "Number of committed taken branches seen by MyTAGE backend"),
      ADD_STAT(missPredictions, statistics::units::Count::get(),
               "Number of branch mispredictions reported by MyTAGE backend"),
      ADD_STAT(correctPredictions, statistics::units::Count::get(),
               "Number of correct predictions reported by MyTAGE backend"),
      ADD_STAT(bimodalSelections, statistics::units::Count::get(),
               "Number of final predictions provided by the bimodal path"),
      ADD_STAT(
          providerSelections, statistics::units::Count::get(),
          "Number of final predictions provided by tagged provider tables"),
      ADD_STAT(
          altSelections, statistics::units::Count::get(),
          "Number of final predictions provided by alternate tagged tables"),
      ADD_STAT(totalAllocations, statistics::units::Count::get(),
               "Number of tagged table allocations performed by MyTAGE"),
      ADD_STAT(bimodalCorrectWhenSelected, statistics::units::Count::get(),
               "Correct predictions when the bimodal path supplied the final "
               "answer"),
      ADD_STAT(providerCorrectWhenSelected, statistics::units::Count::get(),
               "Correct predictions when a provider table supplied the final "
               "answer"),
      ADD_STAT(
          altCorrectWhenSelected, statistics::units::Count::get(),
          "Correct predictions when an alternate table supplied the final "
          "answer"),
      ADD_STAT(noAllocationBecauseOfHit, statistics::units::Count::get(),
               "Branches that needed no allocation because the prediction "
               "already hit"),
      ADD_STAT(noAllocationBecauseOfNoFree, statistics::units::Count::get(),
               "Allocation opportunities lost because no free useful-bit-zero "
               "entry existed"),
      ADD_STAT(noAllocationBecauseOfProviderHigh,
               statistics::units::Count::get(),
               "Allocation opportunities lost because no longer-history "
               "candidate existed"),
      ADD_STAT(noAllocationButProviderZero, statistics::units::Count::get(),
               "Mispredictions where provider useful bit was zero but "
               "allocation still could not proceed"),
      ADD_STAT(speculativeHistoryUpdates, statistics::units::Count::get(),
               "Speculative history updates performed at prediction time"),
      ADD_STAT(
          squashHistoryRestores, statistics::units::Count::get(),
          "History snapshots restored for fully squashed younger branches"),
      ADD_STAT(
          mispredictHistoryRepairs, statistics::units::Count::get(),
          "History repairs performed for the mispredicting branch itself"),
      ADD_STAT(commitTableUpdates, statistics::units::Count::get(),
               "Final table-training updates performed at commit"),
      ADD_STAT(
          placeholdersCreated, statistics::units::Count::get(),
          "Placeholder histories created for branches discovered after fetch"),
      ADD_STAT(
          predictionLookups, statistics::units::Count::get(),
          "Total prediction lookups issued to the MyTAGE multi-entry backend"),
      ADD_STAT(
          alignedPredictionLookups, statistics::units::Count::get(),
          "Prediction lookups where aligned PC differed from instruction PC"),
      ADD_STAT(
          offsetMatches, statistics::units::Count::get(),
          "Multi-entry table ways whose offset matched the instruction PC"),
      ADD_STAT(tagMatches, statistics::units::Count::get(),
               "Multi-entry table ways whose offset and tag matched"),
      ADD_STAT(providerFound, statistics::units::Count::get(),
               "Lookups that found a tagged provider entry"),
      ADD_STAT(altpredFound, statistics::units::Count::get(),
               "Lookups that found a tagged alternate entry"),
      ADD_STAT(bimodalFallbackNoOffset, statistics::units::Count::get(),
               "Lookups falling back to bimodal because no matching offset "
               "existed"),
      ADD_STAT(
          bimodalFallbackNoProvider, statistics::units::Count::get(),
          "Lookups falling back to bimodal because no provider tag matched"),
      ADD_STAT(providerPseudoNewAlloc, statistics::units::Count::get(),
               "Provider entries considered pseudo-newly allocated"),
      ADD_STAT(lineContextsStarted, statistics::units::Count::get(),
               "Aligned fetch-line history contexts started by the wrapper"),
      ADD_STAT(
          lineContextsReused, statistics::units::Count::get(),
          "Lookups reusing the current aligned fetch-line history context"),
      ADD_STAT(
          lineContextsInvalidated, statistics::units::Count::get(),
          "Aligned fetch-line history contexts invalidated by taken/squash"),
      ADD_STAT(lookupSnapshotsCreated, statistics::units::Count::get(),
               "Independent lookup-state snapshots created for "
               "PredictorHistory records"),
      ADD_STAT(historySnapshotsCreated, statistics::units::Count::get(),
               "Independent speculative-history repair snapshots created for "
               "PredictorHistory records"),
      ADD_STAT(providerUseByTable, statistics::units::Count::get(),
               "Per-table provider usage counts"),
      ADD_STAT(providerHitByTable, statistics::units::Count::get(),
               "Per-table correct prediction counts when used as provider"),
      ADD_STAT(altUseByTable, statistics::units::Count::get(),
               "Per-table alternate predictor usage counts"),
      ADD_STAT(altHitByTable, statistics::units::Count::get(),
               "Per-table correct prediction counts when used as alternate "
               "predictor"),
      ADD_STAT(allocationByTable, statistics::units::Count::get(),
               "Per-table allocation counts"),
      ADD_STAT(accuracy, statistics::units::Ratio::get(),
               "Overall branch prediction accuracy",
               correctPredictions / totalBranches),
      ADD_STAT(mispredictionRate, statistics::units::Ratio::get(),
               "Overall branch misprediction rate",
               missPredictions / totalBranches),
      ADD_STAT(takenRate, statistics::units::Ratio::get(),
               "Taken branch ratio at commit", takenBranches / totalBranches),
      ADD_STAT(providerSelectionRate, statistics::units::Ratio::get(),
               "Fraction of predictions answered by provider tables",
               providerSelections / totalBranches),
      ADD_STAT(altSelectionRate, statistics::units::Ratio::get(),
               "Fraction of predictions answered by alternate tables",
               altSelections / totalBranches),
      ADD_STAT(bimodalSelectionRate, statistics::units::Ratio::get(),
               "Fraction of predictions answered by the bimodal path",
               bimodalSelections / totalBranches),
      ADD_STAT(providerAccuracyWhenSelected, statistics::units::Ratio::get(),
               "Provider-table accuracy when provider tables were selected",
               providerCorrectWhenSelected / providerSelections),
      ADD_STAT(altAccuracyWhenSelected, statistics::units::Ratio::get(),
               "Alternate-table accuracy when alternate tables were selected",
               altCorrectWhenSelected / altSelections),
      ADD_STAT(bimodalAccuracyWhenSelected, statistics::units::Ratio::get(),
               "Bimodal-path accuracy when the bimodal path was selected",
               bimodalCorrectWhenSelected / bimodalSelections),
      ADD_STAT(allocationsPerMisprediction, statistics::units::Ratio::get(),
               "Average number of tagged allocations per misprediction",
               totalAllocations / missPredictions)
{
}

void
MyTAGE::MyTAGEStats::regStats()
{
    statistics::Group::regStats();

    using namespace statistics;

    providerUseByTable.init(compCount).flags(nozero);
    providerHitByTable.init(compCount).flags(nozero);
    altUseByTable.init(compCount).flags(nozero);
    altHitByTable.init(compCount).flags(nozero);
    allocationByTable.init(compCount).flags(nozero);

    for (unsigned i = 0; i < compCount; ++i) {
        const auto name = csprintf("table_%u", i);
        providerUseByTable.subname(i, name);
        providerHitByTable.subname(i, name);
        altUseByTable.subname(i, name);
        altHitByTable.subname(i, name);
        allocationByTable.subname(i, name);
    }

    accuracy.precision(6);
    mispredictionRate.precision(6);
    takenRate.precision(6);
    providerSelectionRate.precision(6);
    altSelectionRate.precision(6);
    bimodalSelectionRate.precision(6);
    providerAccuracyWhenSelected.precision(6);
    altAccuracyWhenSelected.precision(6);
    bimodalAccuracyWhenSelected.precision(6);
    allocationsPerMisprediction.precision(6);
}

void
MyTAGE::MyTAGEStats::preDumpStats()
{
    statistics::Group::preDumpStats();

    const auto aggregate = myTage.aggregateBackendStats();

    predictionLookups = aggregate.predictionLookupCount;
    alignedPredictionLookups = aggregate.alignedPredictionLookupCount;
    offsetMatches = aggregate.offsetMatchCount;
    tagMatches = aggregate.tagMatchCount;
    providerFound = aggregate.providerFoundCount;
    altpredFound = aggregate.altpredFoundCount;
    bimodalFallbackNoOffset = aggregate.bimodalFallbackNoOffsetCount;
    bimodalFallbackNoProvider = aggregate.bimodalFallbackNoProviderCount;
    providerPseudoNewAlloc = aggregate.providerPseudoNewAllocCount;

    totalBranches = aggregate.totalBranchCount;
    takenBranches = aggregate.takenBranchCount;
    missPredictions = aggregate.missPredictionCount;
    correctPredictions =
        aggregate.totalBranchCount - aggregate.missPredictionCount;

    bimodalSelections = aggregate.tableUse.bimMapCount;
    providerSelections = std::accumulate(
        aggregate.tableUse.providerMapCount.begin(),
        aggregate.tableUse.providerMapCount.end(), std::uint64_t{0});
    altSelections = std::accumulate(aggregate.tableUse.altPredMapCount.begin(),
                                    aggregate.tableUse.altPredMapCount.end(),
                                    std::uint64_t{0});
    totalAllocations = std::accumulate(
        aggregate.tableUse.allocationMapCount.begin(),
        aggregate.tableUse.allocationMapCount.end(), std::uint64_t{0});

    bimodalCorrectWhenSelected = aggregate.tableUse.bimHitCountWhenBimUsed;
    providerCorrectWhenSelected =
        aggregate.tableUse.providerHitCountWhenProviderUsed;
    altCorrectWhenSelected = aggregate.tableUse.altpredHitCountWhenAltpredUsed;

    noAllocationBecauseOfHit = aggregate.noAllocationBecauseOfHit;
    noAllocationBecauseOfNoFree = aggregate.noAllocationBecauseOfnoFree;
    noAllocationBecauseOfProviderHigh =
        aggregate.noAllocationBecauseOfproviderHigh;
    noAllocationButProviderZero = aggregate.noAllocationButProviderZero;

    for (unsigned i = 0; i < compCount; ++i) {
        providerUseByTable[i] = aggregate.tableUse.providerMapCount[i];
        providerHitByTable[i] = aggregate.tableUse.providerHitCountList[i];
        altUseByTable[i] = aggregate.tableUse.altPredMapCount[i];
        altHitByTable[i] = aggregate.tableUse.altPredHitCountList[i];
        allocationByTable[i] = aggregate.tableUse.allocationMapCount[i];
    }
}

}  // namespace branch_prediction
}  // namespace gem5
