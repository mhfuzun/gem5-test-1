#pragma once

#include <cassert>
#include <cstdint>
#include <iomanip>  // hizalama için
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using addr_t = std::uint64_t;
using idx_t = std::uint64_t;
using ctr2_t = std::uint8_t;

// Avoid names like NULL that collide with platform macros.
enum pred_type_t
{
    PRED_NONE,
    PRED_PROVIDER,
    PRED_ALTPRED,
    PRED_BIMODAL,
    PRED_BIMODAL_ALT
};
enum random_type_t
{
    SIMRAND,
    LFSR
};
enum history_hash_type_t
{
    HISTORY_HASH_FOLDED,
    HISTORY_HASH_CSR
};
enum replacement_mode_t
{
    REPLACEMENT_USEFUL,
    REPLACEMENT_PLRU
};

static const char* random_type_names[] = {"SIMRAND", "LFSR"};
static const char* history_hash_type_names[] = {"FOLDED",
                                                "CIRCULAR_SHIFT_REGISTER"};
static const char* replacement_mode_names[] = {"USEFUL", "PLRU"};

struct table_cfg_t
{
    int depth;
    int usefull_width;
    int ctr_width;
    int tag_width;
    int history_width;
    int pchistory_start;
    int pchistory_width;

    void printSummary(int idx = -1) const
    {
        if (idx >= 0)
            std::cout << "  [Table " << idx << "]\n";
        else
            std::cout << "  [Table]\n";

        std::cout << "    depth          : " << depth << "\n"
                  << "    useful_width   : " << usefull_width << "\n"
                  << "    ctr_width      : " << ctr_width << "\n"
                  << "    tag_width      : " << tag_width << "\n"
                  << "    history_width  : " << history_width << "\n"
                  << "    pchistory_start: " << pchistory_start << "\n"
                  << "    pchistory_width: " << pchistory_width << "\n";
    }
};

struct tage_cfg_t
{
    int bimodal_depth;
    int ghistory_length;
    int pchistory_length;
    int comp_count;
    int entry_per_set = 1;
    int fetch_line_bytes = 16;
    int bimodal_ctrs_per_row = 8;
    int bimodal_offset_shift = 2;
    bool bimodal_use_aligned_addr = true;
    bool tag_use_aligned_addr = true;
    int pc_hash_start_for_idx;
    int pc_hash_width_for_idx;
    int pc_hash_start_for_tag;
    int pc_hash_width_for_tag;
    std::vector<table_cfg_t> table_cfg;
    bool allocate_randomplacement;
    bool allocate_longerThanProvider;
    bool periodicreset;
    int periodicreset_branchperiod;
    random_type_t random_type;
    int lfsr_width;
    std::uint64_t lfsr_seed;
    bool lfsr_misprediction_update;
    history_hash_type_t history_hash_type = HISTORY_HASH_FOLDED;
    replacement_mode_t replacement_mode = REPLACEMENT_USEFUL;
    bool useAltOnNA = false;
    int numUseAltOnNa = 1;
    int useAltOnNaBits = 4;
    int useAltOnNaHashStart = 2;
    int useAltOnNaHashWidth = 0;

    void printSummary() const
    {
        std::cout << "========== TAGE CONFIG ==========\n";

        std::cout << "General:\n";
        std::cout << "  bimodal_depth               : " << bimodal_depth
                  << "\n";
        std::cout << "  ghistory_length             : " << ghistory_length
                  << "\n";
        std::cout << "  pchistory_length            : " << pchistory_length
                  << "\n";
        std::cout << "  comp_count                  : " << comp_count << "\n";
        std::cout << "  entry_per_set               : " << entry_per_set
                  << "\n";
        std::cout << "  fetch_line_bytes            : " << fetch_line_bytes
                  << "\n";
        std::cout << "  bimodal_ctrs_per_row        : " << bimodal_ctrs_per_row
                  << "\n";
        std::cout << "  bimodal_offset_shift        : " << bimodal_offset_shift
                  << "\n";
        std::cout << "  bimodal_use_aligned_addr    : "
                  << (bimodal_use_aligned_addr ? "true" : "false") << "\n";
        std::cout << "  tag_use_aligned_addr        : "
                  << (tag_use_aligned_addr ? "true" : "false") << "\n";
        std::cout << "  random_type                 : "
                  << random_type_names[random_type] << "\n";
        std::cout << "  lfsr_width                  : " << lfsr_width << "\n";
        std::cout << "  lfsr_seed                   : " << lfsr_seed << "\n";
        std::cout << "  lfsr_misprediction_update   : "
                  << (lfsr_misprediction_update ? "true" : "false") << "\n";
        std::cout << "  history_hash_type           : "
                  << history_hash_type_names[history_hash_type] << "\n";
        std::cout << "  replacement_mode            : "
                  << replacement_mode_names[replacement_mode] << "\n";
        std::cout << "  useAltOnNA                  : "
                  << (useAltOnNA ? "true" : "false") << "\n";
        std::cout << "  numUseAltOnNa               : " << numUseAltOnNa
                  << "\n";
        std::cout << "  useAltOnNaBits              : " << useAltOnNaBits
                  << "\n";
        std::cout << "  useAltOnNaHashStart         : " << useAltOnNaHashStart
                  << "\n";
        std::cout << "  useAltOnNaHashWidth         : " << useAltOnNaHashWidth
                  << "\n";

        // EKLENEN KISIM: Struct'ta olup printSummary'de olmayan değişkenler
        std::cout << "\nPlacement & Reset Params:\n";
        std::cout << "  allocate_randomplacement    : "
                  << (allocate_randomplacement ? "true" : "false") << "\n";
        std::cout << "  allocate_longerThanProvider : "
                  << (allocate_longerThanProvider ? "true" : "false") << "\n";
        std::cout << "  periodicreset               : "
                  << (periodicreset ? "true" : "false") << "\n";
        std::cout << "  periodicreset_branchperiod  : "
                  << periodicreset_branchperiod << "\n";

        std::cout << "\nHash Params:\n";
        std::cout << "  pc_hash_start_for_idx       : "
                  << pc_hash_start_for_idx << "\n";
        std::cout << "  pc_hash_width_for_idx       : "
                  << pc_hash_width_for_idx << "\n";
        std::cout << "  pc_hash_start_for_tag       : "
                  << pc_hash_start_for_tag << "\n";
        std::cout << "  pc_hash_width_for_tag       : "
                  << pc_hash_width_for_tag << "\n";

        std::cout << "\nTables (" << table_cfg.size() << "):\n";

        for (size_t i = 0; i < table_cfg.size(); ++i) {
            table_cfg[i].printSummary(i);
        }

        std::cout << "=================================\n";
    }
};

struct statistics_tableUse
{
    std::uint64_t bimMapCount = 0;
    std::uint64_t bimOnlyMapCount = 0;
    std::uint64_t bimAltMapCount = 0;
    std::uint64_t bimHitCount = 0;
    std::uint64_t bimHitCountWhenBimUsed = 0;
    std::uint64_t bimOnlyHitCountWhenUsed = 0;
    std::uint64_t bimAltHitCountWhenUsed = 0;
    std::uint64_t altpredHitCount = 0;
    std::uint64_t altpredHitCountWhenAltpredUsed = 0;
    std::uint64_t providerHitCount = 0;
    std::uint64_t providerHitCountWhenProviderUsed = 0;
    std::uint64_t providerWouldHaveHitWhenAltUsed = 0;
    std::uint64_t altWouldHaveHitWhenProviderUsed = 0;
    std::uint64_t useAltOnNAUseAltCount = 0;
    std::uint64_t useAltOnNAUseProviderCount = 0;
    std::uint64_t useAltOnNAUpdateCount = 0;
    std::uint64_t useAltOnNAAltCorrectUpdateCount = 0;
    std::uint64_t useAltOnNALongestCorrectUpdateCount = 0;
    std::vector<std::uint64_t> providerMapCount;
    std::vector<std::uint64_t> providerHitCountList;
    std::vector<std::uint64_t> altPredMapCount;
    std::vector<std::uint64_t> altPredHitCountList;
    std::vector<std::uint64_t> allocationMapCount;

    void display() const
    {
        std::cout << "  [Table Use]\n";
        std::cout << "    bimMapCount: " << bimMapCount << "\n";
        std::cout << "    bimOnlyMapCount: " << bimOnlyMapCount << "\n";
        std::cout << "    bimAltMapCount: " << bimAltMapCount << "\n";
        std::cout << "    bimHitCount: " << bimHitCount << "\n";
        std::cout << "    bimHitCountWhenBimUsed: " << bimHitCountWhenBimUsed
                  << "\n";
        std::cout << "    bimOnlyHitCountWhenUsed: " << bimOnlyHitCountWhenUsed
                  << "\n";
        std::cout << "    bimAltHitCountWhenUsed: " << bimAltHitCountWhenUsed
                  << "\n";
        std::cout << "    bimHitRateWhenBimUsed: "
                  << (bimMapCount ? ((double)bimHitCountWhenBimUsed /
                                     bimMapCount * 100.0)
                                  : 0.0)
                  << "\n";
        std::cout << "\n";
        std::cout << "    altpredHitCount: " << altpredHitCount << "\n";
        std::cout << "    altpredHitCountWhenAltpredUsed: "
                  << altpredHitCountWhenAltpredUsed << "\n";
        const auto altpred_used = std::accumulate(
            altPredMapCount.begin(), altPredMapCount.end(), std::uint64_t{0});
        std::cout << "    altpredHitRateWhenAltpredUsed: "
                  << (altpred_used ? ((double)altpredHitCountWhenAltpredUsed /
                                      altpred_used * 100.0)
                                   : 0.0)
                  << "\n";
        std::cout << "\n";
        std::cout << "    providerHitCount: " << providerHitCount << "\n";
        std::cout << "    providerHitCountWhenProviderUsed: "
                  << providerHitCountWhenProviderUsed << "\n";
        std::cout << "    providerWouldHaveHitWhenAltUsed: "
                  << providerWouldHaveHitWhenAltUsed << "\n";
        std::cout << "    altWouldHaveHitWhenProviderUsed: "
                  << altWouldHaveHitWhenProviderUsed << "\n";
        const auto provider_used =
            std::accumulate(providerMapCount.begin(), providerMapCount.end(),
                            std::uint64_t{0});
        std::cout << "    providerHitRateWhenProviderUsed: "
                  << (provider_used
                          ? ((double)providerHitCountWhenProviderUsed /
                             provider_used * 100.0)
                          : 0.0)
                  << "\n";
        std::cout << "\n";
        std::cout << "    useAltOnNAUseAltCount: " << useAltOnNAUseAltCount
                  << "\n";
        std::cout << "    useAltOnNAUseProviderCount: "
                  << useAltOnNAUseProviderCount << "\n";
        std::cout << "    useAltOnNAUpdateCount: " << useAltOnNAUpdateCount
                  << "\n";
        std::cout << "    useAltOnNAAltCorrectUpdateCount: "
                  << useAltOnNAAltCorrectUpdateCount << "\n";
        std::cout << "    useAltOnNALongestCorrectUpdateCount: "
                  << useAltOnNALongestCorrectUpdateCount << "\n";
        std::cout << "\n";

        auto printVec = [](const std::string& name,
                           const std::vector<std::uint64_t>& vec) {
            std::cout << "    " << name << ": ";
            for (size_t i = 0; i < vec.size(); ++i) {
                std::cout << vec[i];
                if (i != vec.size() - 1)
                    std::cout << ", ";
            }
            std::cout << "\n";
        };

        auto printVecsRate = [](const std::string& name,
                                const std::vector<std::uint64_t>& vec1,
                                const std::vector<std::uint64_t>& vec2) {
            assert(vec1.size() == vec2.size());

            std::cout << "    " << name << ": ";
            for (size_t i = 0; i < vec1.size(); ++i) {
                if (vec2[i] > 0)
                    std::cout << (double)vec1[i] / vec2[i] * 100.0;
                else
                    std::cout << "0";
                if (i != vec1.size() - 1)
                    std::cout << "%, ";
            }
            std::cout << "%\n";
        };

        printVec("providerMapCount", providerMapCount);
        printVec("providerHitCountList", providerHitCountList);
        printVecsRate("providerHitRateList", providerHitCountList,
                      providerMapCount);
        printVecsRate(
            "providerUseList", providerMapCount,
            std::vector<std::uint64_t>(
                providerMapCount.size(),
                std::accumulate(providerMapCount.begin(),
                                providerMapCount.end(), std::uint64_t{0})));
        printVec("altPredMapCount", altPredMapCount);
        printVec("altPredHitCountList", altPredHitCountList);
        printVecsRate("altPredHitRateList", altPredHitCountList,
                      altPredMapCount);
        printVec("allocationMapCount", allocationMapCount);
        printVecsRate(
            "allocationMapRate", allocationMapCount,
            std::vector<std::uint64_t>(
                allocationMapCount.size(),
                std::accumulate(allocationMapCount.begin(),
                                allocationMapCount.end(), std::uint64_t{0})));
    }
};

struct statistics_t
{
    statistics_tableUse tableUse;

    std::uint64_t predictionLookupCount = 0;
    std::uint64_t alignedPredictionLookupCount = 0;
    std::uint64_t offsetMatchCount = 0;
    std::uint64_t tagMatchCount = 0;
    std::uint64_t providerFoundCount = 0;
    std::uint64_t altpredFoundCount = 0;
    std::uint64_t bimodalFallbackNoOffsetCount = 0;
    std::uint64_t bimodalFallbackNoProviderCount = 0;
    std::uint64_t providerPseudoNewAllocCount = 0;

    std::uint64_t totalBranchCount = 0;
    std::uint64_t takenBranchCount = 0;
    std::uint64_t missPredictionCount = 0;
    std::uint64_t noAllocationBecauseOfHit = 0;
    std::uint64_t noAllocationBecauseOfnoFree = 0;
    std::uint64_t noAllocationBecauseOfproviderHigh = 0;
    std::uint64_t noAllocationButProviderZero = 0;
    std::uint64_t noAllocationBecauseUseAltOnNALongestHit = 0;

    void display() const
    {
        std::cout << "=== Statistics ===\n";
        std::cout << "Prediction Lookup: " << predictionLookupCount << "\n";
        std::cout << "Aligned Prediction Lookup: "
                  << alignedPredictionLookupCount << "\n";
        std::cout << "Offset Match: " << offsetMatchCount << "\n";
        std::cout << "Tag Match: " << tagMatchCount << "\n";
        std::cout << "Provider Found: " << providerFoundCount << "\n";
        std::cout << "AltPred Found: " << altpredFoundCount << "\n";
        std::cout << "Bimodal Fallback No Offset: "
                  << bimodalFallbackNoOffsetCount << "\n";
        std::cout << "Bimodal Fallback No Provider: "
                  << bimodalFallbackNoProviderCount << "\n";
        std::cout << "Provider Pseudo New Alloc: "
                  << providerPseudoNewAllocCount << "\n";
        std::cout << "Total Branch: " << totalBranchCount << "\n";
        std::cout << "Taken Branch: " << takenBranchCount << "\n";
        std::cout << "Miss Prediction: " << missPredictionCount << "\n";

        if (totalBranchCount > 0) {
            double missRate =
                (double)missPredictionCount / totalBranchCount * 100.0;
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "Miss Rate: " << missRate << " %\n";
        }

        std::cout << "\n";
        std::cout << "noAllocationBecauseOfHit: " << noAllocationBecauseOfHit
                  << "\n";
        std::cout << "noAllocationBecauseOfnoFree: "
                  << noAllocationBecauseOfnoFree << "\n";
        std::cout << "noAllocationBecauseOfproviderHigh: "
                  << noAllocationBecauseOfproviderHigh << "\n";
        std::cout << "noAllocationButProviderZero: "
                  << noAllocationButProviderZero << "\n";
        std::cout << "noAllocationBecauseUseAltOnNALongestHit: "
                  << noAllocationBecauseUseAltOnNALongestHit << "\n";

        tableUse.display();
        if (totalBranchCount > 0) {
            double hitRate =
                (double)tableUse.bimHitCount / totalBranchCount * 100.0;
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "BIM Agreement Rate (all branches): " << hitRate
                      << " %\n";
        }
    }
};

/**
 *   getIdx: adrese idx döndürülür.
 *
 *   @param addr adres değeri
 *   @param s s'inci bitten başla
 *   @param w w kadar bir al
 */
[[maybe_unused]] static inline idx_t
getIdx(addr_t addr, int s, int w)
{
    if (w <= 0)
        return 0;

    // word-aligned ise düşük s bitleri at
    addr_t shifted = (s <= 0) ? addr : (addr >> static_cast<addr_t>(s));

    // table size = 2^w → düşük s bitini al
    idx_t mask =
        (w >= 64) ? ~idx_t{0} : ((idx_t{1} << static_cast<idx_t>(w)) - 1);

    return static_cast<idx_t>(shifted & mask);
}

// Returns log2(depth) when depth is a power of two, otherwise
// ceil(log2(depth)).
static inline int
idxWidthForDepth(std::uint64_t depth)
{
    if (depth <= 1)
        return 0;
    std::uint64_t value = depth - 1;
    int width = 0;
    while (value != 0) {
        ++width;
        value >>= 1;
    }
    return width;
}

static inline std::uint64_t
maskForWidth(int width)
{
    if (width <= 0)
        return 0;
    if (width >= 64)
        return ~std::uint64_t{0};
    return (std::uint64_t{1} << static_cast<std::uint64_t>(width)) - 1;
}
