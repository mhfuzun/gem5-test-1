#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "generic.h"
#include "lfsr.h"
#include "tage.h"

namespace
{
constexpr std::size_t invalid_way = static_cast<std::size_t>(-1);

inline std::uint32_t
mid_hi_for_width(int ctr_width)
{
    if (ctr_width <= 0)
        return 0;
    return (1u << static_cast<std::uint32_t>(ctr_width - 1));
}

inline bool
pred_from_ctr(std::uint32_t ctr, int ctr_width)
{
    return ctr >= mid_hi_for_width(ctr_width);
}

inline bool
ctr_is_weak(std::uint32_t ctr, int ctr_width)
{
    const std::uint32_t mid_hi = mid_hi_for_width(ctr_width);
    const std::uint32_t mid_lo = (mid_hi == 0) ? 0u : (mid_hi - 1);
    return (ctr == mid_hi) || (ctr == mid_lo);
}

inline std::int8_t
signed_min_for_width(int ctr_width)
{
    if (ctr_width <= 0)
        return 0;
    return static_cast<std::int8_t>(-(1 << (ctr_width - 1)));
}

inline std::int8_t
signed_max_for_width(int ctr_width)
{
    if (ctr_width <= 0)
        return 0;
    return static_cast<std::int8_t>((1 << (ctr_width - 1)) - 1);
}

inline void
update_signed_counter(std::int8_t& ctr, bool up, int ctr_width)
{
    const std::int8_t min_ctr = signed_min_for_width(ctr_width);
    const std::int8_t max_ctr = signed_max_for_width(ctr_width);
    if (up) {
        if (ctr < max_ctr)
            ++ctr;
    } else {
        if (ctr > min_ctr)
            --ctr;
    }
}

inline int
effective_history_length(const tage_cfg_t& cfg, const table_cfg_t& tc)
{
    return std::max(0, std::min(tc.history_width, cfg.ghistory_length));
}

struct match_t
{
    std::size_t table_idx = invalid_way;
    std::size_t way = invalid_way;
    int history_width = 0;
};

inline bool
better_match(const match_t& lhs, const match_t& rhs)
{
    if (lhs.history_width != rhs.history_width)
        return lhs.history_width > rhs.history_width;
    return lhs.table_idx > rhs.table_idx;
}

inline bool
better_allocation_target(const tage_cfg_t& cfg, int lhs, int rhs)
{
    const auto& ltc = cfg.table_cfg[static_cast<std::size_t>(lhs)];
    const auto& rtc = cfg.table_cfg[static_cast<std::size_t>(rhs)];
    if (ltc.history_width != rtc.history_width)
        return ltc.history_width > rtc.history_width;
    return lhs > rhs;
}
}  // namespace

bool
tage::provider_exists(const TAGE_State& state)
{
    return (state.provider_idx != static_cast<std::size_t>(-1));
}

bool
tage::altpred_exists(const TAGE_State& state)
{
    return (state.altpred_idx != static_cast<std::size_t>(-1));
}

void
tage::initStatisticsStorage()
{
    stats = statistics_t{};
    stats.tableUse.providerMapCount.assign(
        static_cast<std::size_t>(tage_cfg.comp_count), 0);
    stats.tableUse.altPredMapCount.assign(
        static_cast<std::size_t>(tage_cfg.comp_count), 0);
    stats.tableUse.allocationMapCount.assign(
        static_cast<std::size_t>(tage_cfg.comp_count), 0);
    stats.tableUse.providerHitCountList.assign(
        static_cast<std::size_t>(tage_cfg.comp_count), 0);
    stats.tableUse.altPredHitCountList.assign(
        static_cast<std::size_t>(tage_cfg.comp_count), 0);
}

tage::tage(const tage_cfg_t& cfg)
    : tage_cfg(cfg), component_list(), ghistory_reg(cfg.ghistory_length),
      pchistory(cfg.pchistory_length),
      bimodal_predictor(
          static_cast<std::size_t>(cfg.bimodal_depth > 0 ? cfg.bimodal_depth
                                                         : 0),
          static_cast<std::size_t>(
              cfg.bimodal_ctrs_per_row > 0 ? cfg.bimodal_ctrs_per_row : 1),
          cfg.bimodal_offset_shift,
          static_cast<std::size_t>(
              cfg.fetch_line_bytes > 0 ? cfg.fetch_line_bytes : 0)),
      lfsr_reg((cfg.random_type == LFSR && cfg.lfsr_width > 0) ? cfg.lfsr_width
                                                               : 1,
               cfg.lfsr_seed)
{
    if (cfg.bimodal_depth <= 0)
        throw std::invalid_argument("tage_cfg.bimodal_depth must be > 0");
    if (cfg.ghistory_length < 0)
        throw std::invalid_argument("tage_cfg.ghistory_length must be >= 0");
    if (cfg.pchistory_length < 0)
        throw std::invalid_argument("tage_cfg.pchistory_length must be >= 0");
    if (cfg.comp_count < 0)
        throw std::invalid_argument("tage_cfg.comp_count must be >= 0");
    if (cfg.entry_per_set <= 0)
        throw std::invalid_argument("tage_cfg.entry_per_set must be > 0");
    if (cfg.fetch_line_bytes <= 0)
        throw std::invalid_argument("tage_cfg.fetch_line_bytes must be > 0");
    if (cfg.bimodal_ctrs_per_row <= 0)
        throw std::invalid_argument(
            "tage_cfg.bimodal_ctrs_per_row must be > 0");
    if (cfg.bimodal_offset_shift < 0 || cfg.bimodal_offset_shift >= 64)
        throw std::invalid_argument(
            "tage_cfg.bimodal_offset_shift must be in [0, 63]");
    if (cfg.random_type == LFSR &&
        (cfg.lfsr_width <= 0 || cfg.lfsr_width > 64))
        throw std::invalid_argument(
            "tage_cfg.lfsr_width must be in [1, 64] when random_type is LFSR");
    if (cfg.numUseAltOnNa <= 0)
        throw std::invalid_argument("tage_cfg.numUseAltOnNa must be > 0");
    if (cfg.useAltOnNaBits <= 0 || cfg.useAltOnNaBits > 8)
        throw std::invalid_argument(
            "tage_cfg.useAltOnNaBits must be in [1, 8]");
    if (cfg.useAltOnNaHashStart < 0 || cfg.useAltOnNaHashWidth < 0)
        throw std::invalid_argument(
            "tage_cfg useAltOnNa hash parameters must be >= 0");
    if (static_cast<std::size_t>(cfg.comp_count) > cfg.table_cfg.size())
        throw std::invalid_argument("tage_cfg.table_cfg size < comp_count");

    useAltPredForNewlyAllocated.assign(
        static_cast<std::size_t>(cfg.numUseAltOnNa), 0);

    component_list.reserve(static_cast<std::size_t>(cfg.comp_count));
    idx_history_hash.reserve(static_cast<std::size_t>(cfg.comp_count));
    tag_history_hash.reserve(static_cast<std::size_t>(cfg.comp_count));

    for (int i = 0; i < cfg.comp_count; ++i) {
        const auto& tc = cfg.table_cfg[static_cast<std::size_t>(i)];
        if (tc.depth <= 0)
            throw std::invalid_argument("table_cfg.depth must be > 0");
        if (tc.ctr_width <= 0)
            throw std::invalid_argument("table_cfg.ctr_width must be > 0");
        if (tc.tag_width <= 0)
            throw std::invalid_argument("table_cfg.tag_width must be > 0");
        if (tc.usefull_width <= 0)
            throw std::invalid_argument("table_cfg.usefull_width must be > 0");
        if (tc.history_width < 0)
            throw std::invalid_argument(
                "table_cfg.history_width must be >= 0");

        component_list.emplace_back(tc, cfg.entry_per_set);

        const int history_len = std::max(1, effective_history_length(cfg, tc));
        const int idx_width = std::max(
            1, idxWidthForDepth(static_cast<std::uint64_t>(tc.depth)));
        const int tag_width = std::max(1, tc.tag_width);
        idx_history_hash.emplace_back(idx_width, history_len);
        tag_history_hash.emplace_back(tag_width, history_len);
    }

    if (!cfg.table_cfg.empty())
        usefull_reset_flipState =
            std::max(0, cfg.table_cfg[0].usefull_width - 1);

    initStatisticsStorage();
}

std::uint32_t
tage::pc_offset(addr_t addr) const
{
    return static_cast<std::uint32_t>(
        addr % static_cast<addr_t>(tage_cfg.fetch_line_bytes));
}

void
tage::snapshotState(TAGE_State& state) const
{
    state = TAGE_State();
    state.ghistSnapshot = ghistory_reg;
    state.pchistSnapshot = pchistory;
    state.idxHistoryHashSnapshot = idx_history_hash;
    state.tagHistoryHashSnapshot = tag_history_hash;
    state.usefull_reset_ctr = usefull_reset_ctr;
    state.usefull_reset_flipState = usefull_reset_flipState;
    state.comp_idx.assign(static_cast<std::size_t>(tage_cfg.comp_count), 0);
    state.comp_tag.assign(static_cast<std::size_t>(tage_cfg.comp_count), 0);
    state.comp_row.assign(static_cast<std::size_t>(tage_cfg.comp_count),
                          nullptr);
    state.comp_way.assign(static_cast<std::size_t>(tage_cfg.comp_count),
                          invalid_way);
}

void
tage::resetState(const TAGE_State& state)
{
    ghistory_reg = state.ghistSnapshot;
    pchistory = state.pchistSnapshot;
    idx_history_hash = state.idxHistoryHashSnapshot;
    tag_history_hash = state.tagHistoryHashSnapshot;
    usefull_reset_ctr = state.usefull_reset_ctr;
    usefull_reset_flipState = state.usefull_reset_flipState;
}

const statistics_t&
tage::getStats() const
{
    return stats;
}

void
tage::resetStats()
{
    initStatisticsStorage();
    lastStateValid = false;
}

std::uint64_t
tage::global_history_hash(const TAGE_State& state, std::size_t table_idx,
                          int width) const
{
    if (width <= 0)
        return 0;

    const auto& tc = tage_cfg.table_cfg[table_idx];
    if (effective_history_length(tage_cfg, tc) == 0)
        return 0;

    if (tage_cfg.history_hash_type == HISTORY_HASH_CSR) {
        const int idx_width =
            idxWidthForDepth(static_cast<std::uint64_t>(tc.depth));
        if (width == idx_width &&
            table_idx < state.idxHistoryHashSnapshot.size())
            return state.idxHistoryHashSnapshot[table_idx].getHash() &
                   maskForWidth(width);
        if (width == tc.tag_width &&
            table_idx < state.tagHistoryHashSnapshot.size())
            return state.tagHistoryHashSnapshot[table_idx].getHash() &
                   maskForWidth(width);
    }

    return static_cast<std::uint64_t>(state.ghistSnapshot.hash_foldedHistory(
        static_cast<std::size_t>(tc.history_width), width));
}

std::size_t
tage::use_alt_on_na_index(addr_t addr) const
{
    const std::size_t table_size = useAltPredForNewlyAllocated.empty()
                                       ? std::size_t{1}
                                       : useAltPredForNewlyAllocated.size();
    const int hash_width =
        (tage_cfg.useAltOnNaHashWidth > 0)
            ? tage_cfg.useAltOnNaHashWidth
            : idxWidthForDepth(static_cast<std::uint64_t>(table_size));
    return static_cast<std::size_t>(
        getIdx(addr, tage_cfg.useAltOnNaHashStart, hash_width) %
        static_cast<idx_t>(table_size));
}

bool
tage::use_alt_on_na_prefers_alt(std::size_t idx) const
{
    if (useAltPredForNewlyAllocated.empty())
        return true;
    return useAltPredForNewlyAllocated[idx %
                                       useAltPredForNewlyAllocated.size()] >=
           0;
}

void
tage::update_use_alt_on_na(const TAGE_State& state, bool alt_correct)
{
    if (useAltPredForNewlyAllocated.empty())
        return;
    auto& ctr =
        useAltPredForNewlyAllocated[state.use_alt_on_na_idx %
                                    useAltPredForNewlyAllocated.size()];
    update_signed_counter(ctr, alt_correct, tage_cfg.useAltOnNaBits);
}

void
tage::record_prediction_use(const TAGE_State& state)
{
    switch (state.predType) {
        case PRED_PROVIDER:
            if (provider_exists(state))
                stats.tableUse.providerMapCount[state.provider_idx]++;
            break;
        case PRED_ALTPRED:
            if (altpred_exists(state))
                stats.tableUse.altPredMapCount[state.altpred_idx]++;
            break;
        case PRED_BIMODAL:
            stats.tableUse.bimMapCount++;
            stats.tableUse.bimOnlyMapCount++;
            break;
        case PRED_BIMODAL_ALT:
            stats.tableUse.bimMapCount++;
            stats.tableUse.bimAltMapCount++;
            break;
        case PRED_NONE:
            break;
    }

    if (state.use_alt_on_na_active) {
        if (state.predType == PRED_PROVIDER)
            stats.tableUse.useAltOnNAUseProviderCount++;
        else
            stats.tableUse.useAltOnNAUseAltCount++;
    }
}

void
tage::record_prediction_hits(const TAGE_State& state, bool outcome)
{
    if (outcome == state.bimpred_res)
        stats.tableUse.bimHitCount++;
    if (outcome == state.bimpred_res && (state.predType == PRED_BIMODAL ||
                                         state.predType == PRED_BIMODAL_ALT)) {
        stats.tableUse.bimHitCountWhenBimUsed++;
        if (state.predType == PRED_BIMODAL)
            stats.tableUse.bimOnlyHitCountWhenUsed++;
        else
            stats.tableUse.bimAltHitCountWhenUsed++;
    }

    if (altpred_exists(state)) {
        if (outcome == state.altpred_res)
            stats.tableUse.altpredHitCount++;
        if (outcome == state.altpred_res && state.predType == PRED_ALTPRED) {
            stats.tableUse.altpredHitCountWhenAltpredUsed++;
            stats.tableUse.altPredHitCountList[state.altpred_idx]++;
        }
    }

    if (provider_exists(state)) {
        if (outcome == state.providerpred_res)
            stats.tableUse.providerHitCount++;
        if (outcome == state.providerpred_res &&
            state.predType == PRED_PROVIDER) {
            stats.tableUse.providerHitCountWhenProviderUsed++;
            stats.tableUse.providerHitCountList[state.provider_idx]++;
        }
        if ((state.predType == PRED_ALTPRED ||
             state.predType == PRED_BIMODAL_ALT) &&
            outcome == state.providerpred_res) {
            stats.tableUse.providerWouldHaveHitWhenAltUsed++;
        }
        if (state.predType == PRED_PROVIDER && outcome == state.altpred_res) {
            stats.tableUse.altWouldHaveHitWhenProviderUsed++;
        }
    }
}

bool
tage::getPrediction(addr_t addr, TAGE_State& state)
{
    TAGE_State history_state;
    snapshotState(history_state);
    return getPrediction(addr, addr, history_state, state);
}

bool
tage::getPrediction(addr_t index_addr, addr_t lookup_addr,
                    const TAGE_State& history_state, TAGE_State& state)
{
    state = history_state;
    state.provider_idx = invalid_way;
    state.altpred_idx = invalid_way;
    state.provider_way = invalid_way;
    state.altpred_way = invalid_way;
    state.predType = PRED_NONE;
    state.altpred_res = 0;
    state.providerpred_res = 0;
    state.provider_pseudo_new_alloc = false;
    state.altpred_is_bimodal = false;
    state.use_alt_on_na_active = false;
    state.use_alt_on_na_idx = 0;

    const std::size_t n = static_cast<std::size_t>(tage_cfg.comp_count);
    state.comp_idx.assign(n, 0);
    state.comp_tag.assign(n, 0);
    state.comp_row.assign(n, nullptr);
    state.comp_way.assign(n, invalid_way);
    state.index_addr = index_addr;
    state.lookup_addr = lookup_addr;

    stats.predictionLookupCount++;
    if (index_addr != lookup_addr)
        stats.alignedPredictionLookupCount++;

    const addr_t bimodal_addr =
        tage_cfg.bimodal_use_aligned_addr ? index_addr : lookup_addr;
    state.bimodal_addr = bimodal_addr;
    state.bimodal_lookup_addr = lookup_addr;
    state.bimodal_ctr =
        bimodal_predictor.getPrediction(bimodal_addr, lookup_addr);
    state.bimpred_res = pred_from_ctr(state.bimodal_ctr, 2);

    std::vector<match_t> matches;
    matches.reserve(n);
    const std::uint32_t fetch_offset = pc_offset(lookup_addr);
    const addr_t tag_addr =
        tage_cfg.tag_use_aligned_addr ? index_addr : lookup_addr;
    state.tag_addr = tag_addr;
    bool offset_exists = false;

    for (int i = 0; i < tage_cfg.comp_count; ++i) {
        std::uint64_t idx_hash = 0;
        std::uint64_t tag_hash = 0;

        const auto table_idx = static_cast<std::size_t>(i);
        const auto& tc = tage_cfg.table_cfg[table_idx];
        const int idx_width =
            idxWidthForDepth(static_cast<std::uint64_t>(tc.depth));
        const std::size_t path_start =
            (tc.pchistory_start > 0)
                ? static_cast<std::size_t>(tc.pchistory_start)
                : 0u;
        const std::size_t path_width =
            (tc.pchistory_width > 0)
                ? static_cast<std::size_t>(tc.pchistory_width)
                : 0u;

        idx_hash ^=
            getIdx(index_addr,
                   (tage_cfg.pc_hash_start_for_idx + tage_cfg.comp_count - i),
                   tage_cfg.pc_hash_width_for_idx);
        idx_hash ^= global_history_hash(state, table_idx, idx_width);
        if (tage_cfg.pchistory_length != 0 && path_width > 0) {
            const int path_idx_width =
                std::max(1, std::min(idx_width, tc.pchistory_width));
            idx_hash ^= static_cast<std::uint64_t>(
                state.pchistSnapshot.hash_foldedHistory(path_start, path_width,
                                                        path_idx_width));
        }
        idx_hash &= maskForWidth(idx_width);

        tag_hash ^= getIdx(tag_addr, tage_cfg.pc_hash_start_for_tag,
                           tage_cfg.pc_hash_width_for_tag);
        tag_hash ^= global_history_hash(state, table_idx, tc.tag_width);
        if (tage_cfg.pchistory_length != 0 && path_width > 0) {
            const int path_tag_width =
                std::max(1, std::min(tc.tag_width, tc.pchistory_width));
            tag_hash ^= static_cast<std::uint64_t>(
                state.pchistSnapshot.hash_foldedHistory(path_start, path_width,
                                                        path_tag_width));
        }
        tag_hash &= maskForWidth(tc.tag_width);

        state.comp_idx[table_idx] = static_cast<idx_t>(idx_hash);
        state.comp_tag[table_idx] = tag_hash;

        tage_comp_t* best_row = nullptr;
        std::size_t best_way = invalid_way;

        for (std::size_t way = 0; way < component_list[table_idx].way_count();
             ++way) {
            auto& row = component_list[table_idx].row(
                static_cast<idx_t>(idx_hash), way);
            if (!row.valid || row.offset != fetch_offset)
                continue;

            offset_exists = true;
            stats.offsetMatchCount++;

            if ((tag_hash & maskForWidth(tc.tag_width)) !=
                (row.tag & maskForWidth(tc.tag_width))) {
                continue;
            }

            stats.tagMatchCount++;
            best_row = &row;
            best_way = way;
            break;
        }

        if (best_row) {
            state.comp_row[table_idx] = best_row;
            state.comp_way[table_idx] = best_way;
            matches.push_back(match_t{table_idx, best_way, tc.history_width});
        }
    }

    if (!offset_exists) {
        stats.bimodalFallbackNoOffsetCount++;
        state.predType = PRED_BIMODAL;
        state.pred = state.bimpred_res;
        return state.pred;
    }

    match_t provider_match;
    bool have_provider = false;
    for (const auto& match : matches) {
        if (!have_provider || better_match(match, provider_match)) {
            provider_match = match;
            have_provider = true;
        }
    }

    if (have_provider) {
        state.provider_idx = provider_match.table_idx;
        state.provider_way = provider_match.way;
        stats.providerFoundCount++;
    }

    match_t alt_match;
    bool have_alt = false;
    for (const auto& match : matches) {
        if (match.table_idx == state.provider_idx &&
            match.way == state.provider_way) {
            continue;
        }
        if (!have_alt || better_match(match, alt_match)) {
            alt_match = match;
            have_alt = true;
        }
    }

    if (have_alt) {
        state.altpred_idx = alt_match.table_idx;
        state.altpred_way = alt_match.way;
        stats.altpredFoundCount++;
    }

    if (!provider_exists(state)) {
        stats.bimodalFallbackNoProviderCount++;
        state.predType = PRED_BIMODAL;
        state.pred = state.bimpred_res;
        return state.pred;
    }

    const auto& ptc = tage_cfg.table_cfg[state.provider_idx];
    const std::uint32_t pctr = state.comp_row[state.provider_idx]->ctr;
    state.providerpred_res = pred_from_ctr(pctr, ptc.ctr_width);
    state.provider_pseudo_new_alloc = ctr_is_weak(pctr, ptc.ctr_width);
    if (state.provider_pseudo_new_alloc)
        stats.providerPseudoNewAllocCount++;

    if (altpred_exists(state)) {
        const auto& atc = tage_cfg.table_cfg[state.altpred_idx];
        const std::uint32_t actr = state.comp_row[state.altpred_idx]->ctr;
        state.altpred_res = pred_from_ctr(actr, atc.ctr_width);
    } else {
        state.altpred_res = state.bimpred_res;
        state.altpred_is_bimodal = true;
    }

    bool choose_provider = true;
    if (state.provider_pseudo_new_alloc) {
        if (tage_cfg.useAltOnNA) {
            state.use_alt_on_na_idx = use_alt_on_na_index(lookup_addr);
            state.use_alt_on_na_active = true;
            choose_provider =
                !use_alt_on_na_prefers_alt(state.use_alt_on_na_idx);
        } else {
            choose_provider = false;
        }
    }

    if (choose_provider) {
        state.predType = PRED_PROVIDER;
        state.pred = state.providerpred_res;
        return state.pred;
    }

    if (altpred_exists(state)) {
        state.predType = PRED_ALTPRED;
        state.pred = state.altpred_res;
        return state.pred;
    }

    state.predType = PRED_BIMODAL_ALT;
    state.pred = state.bimpred_res;
    return state.pred;
}

bool
tage::getPrediction(addr_t addr)
{
    lastStateValid = true;
    return getPrediction(addr, lastState);
}

std::vector<int>
tage::allocation_candidates(const TAGE_State& state) const
{
    std::vector<int> candidates;
    candidates.reserve(static_cast<std::size_t>(tage_cfg.comp_count));

    const int provider_history =
        provider_exists(state)
            ? tage_cfg.table_cfg[state.provider_idx].history_width
            : -1;

    for (int i = 0; i < tage_cfg.comp_count; ++i) {
        const auto si = static_cast<std::size_t>(i);
        if (provider_exists(state) && si == state.provider_idx)
            continue;
        if (provider_exists(state) && tage_cfg.allocate_longerThanProvider &&
            tage_cfg.table_cfg[si].history_width <= provider_history)
            continue;
        candidates.push_back(i);
    }

    return candidates;
}

void
tage::update_history_hashes(bool outcome)
{
    if (tage_cfg.history_hash_type == HISTORY_HASH_CSR) {
        for (int i = 0; i < tage_cfg.comp_count; ++i) {
            const auto table_idx = static_cast<std::size_t>(i);
            const int history_len = effective_history_length(
                tage_cfg, tage_cfg.table_cfg[table_idx]);
            if (history_len <= 0)
                continue;

            const bool old_bit =
                ghistory_reg.bit(static_cast<std::size_t>(history_len - 1));
            idx_history_hash[table_idx].updateHash(outcome, old_bit);
            tag_history_hash[table_idx].updateHash(outcome, old_bit);
        }
    }

    ghistory_reg.push(outcome);
}

void
tage::updateHist(addr_t addr, bool outcome, const TAGE_State& state)
{
    (void)state;
    update_history_hashes(outcome);

    if (tage_cfg.pchistory_length != 0)
        pchistory.push((((addr >> 2) ^ (addr >> 5) ^ (addr >> 11)) & 1u) != 0);
}

void
tage::update(addr_t addr, bool outcome)
{
    if (!lastStateValid)
        (void)getPrediction(addr, lastState);

    update(addr, outcome, lastState);
    updateHist(addr, outcome, lastState);
    lastStateValid = false;
}

void
tage::update(addr_t addr, bool outcome, const TAGE_State& state)
{
    if (tage_cfg.replacement_mode == REPLACEMENT_PLRU &&
        state.predType == PRED_PROVIDER && provider_exists(state)) {
        component_list[state.provider_idx].touch(
            state.comp_idx[state.provider_idx], state.provider_way);
    } else if (tage_cfg.replacement_mode == REPLACEMENT_PLRU &&
               state.predType == PRED_ALTPRED && altpred_exists(state)) {
        component_list[state.altpred_idx].touch(
            state.comp_idx[state.altpred_idx], state.altpred_way);
    }

    const int comp_count = tage_cfg.comp_count;
    if (comp_count > 0) {
        const bool prediction_wrong = (state.pred != outcome);
        bool need_alloc = prediction_wrong;

        if (tage_cfg.useAltOnNA && provider_exists(state) &&
            state.provider_pseudo_new_alloc) {
            if (state.providerpred_res == outcome && need_alloc) {
                need_alloc = false;
                stats.noAllocationBecauseUseAltOnNALongestHit++;
            }
            if (state.providerpred_res != state.altpred_res) {
                const bool alt_correct = (state.altpred_res == outcome);
                update_use_alt_on_na(state, alt_correct);
                stats.tableUse.useAltOnNAUpdateCount++;
                if (alt_correct)
                    stats.tableUse.useAltOnNAAltCorrectUpdateCount++;
                else if (state.providerpred_res == outcome)
                    stats.tableUse.useAltOnNALongestCorrectUpdateCount++;
            }
        }

        if (provider_exists(state) && !prediction_wrong)
            stats.noAllocationBecauseOfHit++;

        if (need_alloc) {
            const auto candidates = allocation_candidates(state);

            if (candidates.empty()) {
                stats.noAllocationBecauseOfproviderHigh++;
                if (provider_exists(state) &&
                    state.comp_row[state.provider_idx] &&
                    state.comp_row[state.provider_idx]->u == 0) {
                    stats.noAllocationButProviderZero++;
                }
            } else if (tage_cfg.replacement_mode == REPLACEMENT_PLRU) {
                int alloc_comp_idx = candidates.front();
                if (tage_cfg.allocate_randomplacement &&
                    candidates.size() > 1) {
                    std::size_t target = 0;
                    if (tage_cfg.random_type == SIMRAND) {
                        target = static_cast<std::size_t>(std::rand()) %
                                 candidates.size();
                    } else {
                        lfsr_reg.nextRand();
                        const int candidate_width = std::max(
                            1, idxWidthForDepth(static_cast<std::uint64_t>(
                                   candidates.size())));
                        target = static_cast<std::size_t>(
                            lfsr_reg.foldedhash(candidate_width) %
                            static_cast<std::uint64_t>(candidates.size()));
                    }
                    alloc_comp_idx = candidates[target];
                } else {
                    for (const int candidate : candidates) {
                        if (better_allocation_target(tage_cfg, candidate,
                                                     alloc_comp_idx)) {
                            alloc_comp_idx = candidate;
                        }
                    }
                }

                const auto alloc_idx =
                    static_cast<std::size_t>(alloc_comp_idx);
                const auto& tc = tage_cfg.table_cfg[alloc_idx];
                auto& component = component_list[alloc_idx];
                const std::size_t way =
                    component.victim_way(state.comp_idx[alloc_idx]);
                auto& entry = component.row(state.comp_idx[alloc_idx], way);

                const std::uint32_t mid_hi = mid_hi_for_width(tc.ctr_width);
                const std::uint32_t mid_lo = (mid_hi == 0) ? 0u : (mid_hi - 1);

                entry.u = 0;
                entry.ctr =
                    static_cast<std::uint8_t>(outcome ? mid_hi : mid_lo);
                entry.tag = state.comp_tag[alloc_idx];
                entry.offset =
                    pc_offset(state.lookup_addr ? state.lookup_addr : addr);
                entry.valid = true;
                component.touch(state.comp_idx[alloc_idx], way);

                stats.tableUse.allocationMapCount[alloc_idx]++;
            } else {
                std::vector<int> allocatable_candidates;
                allocatable_candidates.reserve(candidates.size());
                for (const int candidate : candidates) {
                    std::size_t ignored_way = invalid_way;
                    const auto candidate_idx =
                        static_cast<std::size_t>(candidate);
                    if (component_list[candidate_idx].useful_allocation_way(
                            state.comp_idx[candidate_idx], ignored_way)) {
                        allocatable_candidates.push_back(candidate);
                    }
                }

                if (allocatable_candidates.empty()) {
                    for (const int candidate : candidates) {
                        const auto candidate_idx =
                            static_cast<std::size_t>(candidate);
                        component_list[candidate_idx].decay_useful_bits(
                            state.comp_idx[candidate_idx]);
                    }
                    stats.noAllocationBecauseOfnoFree++;
                } else {
                    int alloc_comp_idx = allocatable_candidates.front();
                    if (tage_cfg.allocate_randomplacement &&
                        allocatable_candidates.size() > 1) {
                        std::size_t target = 0;
                        if (tage_cfg.random_type == SIMRAND) {
                            target = static_cast<std::size_t>(std::rand()) %
                                     allocatable_candidates.size();
                        } else {
                            lfsr_reg.nextRand();
                            const int candidate_width = std::max(
                                1, idxWidthForDepth(static_cast<std::uint64_t>(
                                       allocatable_candidates.size())));
                            target = static_cast<std::size_t>(
                                lfsr_reg.foldedhash(candidate_width) %
                                static_cast<std::uint64_t>(
                                    allocatable_candidates.size()));
                        }
                        alloc_comp_idx = allocatable_candidates[target];
                    } else {
                        for (const int candidate : allocatable_candidates) {
                            if (better_allocation_target(tage_cfg, candidate,
                                                         alloc_comp_idx)) {
                                alloc_comp_idx = candidate;
                            }
                        }
                    }

                    const auto alloc_idx =
                        static_cast<std::size_t>(alloc_comp_idx);
                    const auto& tc = tage_cfg.table_cfg[alloc_idx];
                    auto& component = component_list[alloc_idx];
                    std::size_t way = invalid_way;
                    if (!component.useful_allocation_way(
                            state.comp_idx[alloc_idx], way)) {
                        throw std::logic_error(
                            "allocatable candidate lost its replacement way");
                    }
                    auto& entry =
                        component.row(state.comp_idx[alloc_idx], way);

                    const std::uint32_t mid_hi =
                        mid_hi_for_width(tc.ctr_width);
                    const std::uint32_t mid_lo =
                        (mid_hi == 0) ? 0u : (mid_hi - 1);

                    entry.u = 0;
                    entry.ctr =
                        static_cast<std::uint8_t>(outcome ? mid_hi : mid_lo);
                    entry.tag = state.comp_tag[alloc_idx];
                    entry.offset = pc_offset(
                        state.lookup_addr ? state.lookup_addr : addr);
                    entry.valid = true;

                    stats.tableUse.allocationMapCount[alloc_idx]++;
                }
            }
        }
    }

    if (tage_cfg.replacement_mode == REPLACEMENT_USEFUL &&
        provider_exists(state)) {
        const auto& ptc = tage_cfg.table_cfg[state.provider_idx];

        const bool alt_pred = state.altpred_res;

        if (state.providerpred_res != alt_pred && ptc.usefull_width > 0) {
            if (state.providerpred_res == outcome) {
                SaturatingCounter::update_bits(
                    state.comp_row[state.provider_idx]->u, true,
                    ptc.usefull_width);
            } else {
                SaturatingCounter::update_bits(
                    state.comp_row[state.provider_idx]->u, false,
                    ptc.usefull_width);
            }
        }
    }

    if (provider_exists(state)) {
        SaturatingCounter::update_bits(
            state.comp_row[state.provider_idx]->ctr, outcome,
            tage_cfg.table_cfg[state.provider_idx].ctr_width);
    } else {
        auto bimodal_ctr = state.bimodal_ctr;
        SaturatingCounter::update_bits(bimodal_ctr, outcome, 2);
        bimodal_predictor.update(state.bimodal_addr, state.bimodal_lookup_addr,
                                 bimodal_ctr);
    }

    if (tage_cfg.replacement_mode == REPLACEMENT_USEFUL &&
        tage_cfg.periodicreset && !component_list.empty() &&
        tage_cfg.periodicreset_branchperiod > 0) {
        usefull_reset_ctr++;
        if (usefull_reset_ctr ==
            static_cast<std::uint64_t>(tage_cfg.periodicreset_branchperiod)) {
            for (auto& comp : component_list)
                comp.reset_usefull_bits(usefull_reset_flipState);

            usefull_reset_ctr = 0;
            usefull_reset_flipState--;
            if (usefull_reset_flipState < 0) {
                usefull_reset_flipState =
                    std::max(0, tage_cfg.table_cfg[0].usefull_width - 1);
            }
        }
    }

    if (tage_cfg.random_type == LFSR && tage_cfg.lfsr_misprediction_update) {
        const addr_t update_addr =
            state.lookup_addr ? state.lookup_addr : addr;
        addr_t hash = update_addr ^ (update_addr >> 3) ^ (update_addr << 5);

        if (outcome != state.pred)
            lfsr_reg.updateRand(static_cast<std::uint64_t>(hash));
    }

    record_prediction_use(state);

    stats.totalBranchCount++;
    if (outcome)
        stats.takenBranchCount++;
    if (outcome != state.pred)
        stats.missPredictionCount++;

    record_prediction_hits(state, outcome);
}

void
tage::DisplayStatistics(void)
{
    stats.display();
}

tage_cmp::tage_cmp(table_cfg_t cfg, int entry_per_set)
    : table_cfg(cfg),
      ways(entry_per_set > 0 ? static_cast<std::size_t>(entry_per_set) : 1),
      table((cfg.depth > 0 ? static_cast<std::size_t>(cfg.depth) : 0) * ways),
      plru_table(cfg.depth > 0 ? static_cast<std::size_t>(cfg.depth) : 0,
                 std::vector<std::uint8_t>(ways, 0))
{
    if (cfg.depth <= 0)
        throw std::invalid_argument("tage_cmp.depth must be > 0");
    if (entry_per_set <= 0)
        throw std::invalid_argument("tage_cmp.entry_per_set must be > 0");

    for (auto& entry : table)
        entry.valid = false;
}

void
tage_cmp::reset_usefull_bits(int bit_index)
{
    if (bit_index < 0 || bit_index >= 8)
        return;
    for (auto& entry : table) {
        entry.u &= static_cast<std::uint8_t>(
            ~(std::uint8_t{1} << static_cast<unsigned>(bit_index)));
    }
}

std::size_t
tage_cmp::set_index(idx_t idx) const
{
    return static_cast<std::size_t>(idx % static_cast<idx_t>(table_cfg.depth));
}

std::size_t
tage_cmp::flat_index(idx_t idx, std::size_t way) const
{
    if (way >= ways)
        throw std::out_of_range("tage_cmp way is out of range");
    return set_index(idx) * ways + way;
}

const tage_comp_t&
tage_cmp::row(idx_t idx) const
{
    return row(idx, 0);
}

tage_comp_t&
tage_cmp::row(idx_t idx)
{
    return row(idx, 0);
}

const tage_comp_t&
tage_cmp::row(idx_t idx, std::size_t way) const
{
    return table[flat_index(idx, way)];
}

tage_comp_t&
tage_cmp::row(idx_t idx, std::size_t way)
{
    return table[flat_index(idx, way)];
}

std::size_t
tage_cmp::victim_way(idx_t idx) const
{
    const auto& ranks = plru_table[set_index(idx)];
    std::size_t victim = 0;
    for (std::size_t way = 1; way < ranks.size(); ++way) {
        if (ranks[way] < ranks[victim])
            victim = way;
    }
    return victim;
}

bool
tage_cmp::useful_allocation_way(idx_t idx, std::size_t& way) const
{
    const std::size_t set = set_index(idx);

    for (std::size_t candidate = 0; candidate < ways; ++candidate) {
        const auto& entry = table[set * ways + candidate];
        if (!entry.valid || entry.u == 0) {
            way = candidate;
            return true;
        }
    }

    return false;
}

void
tage_cmp::decay_useful_bits(idx_t idx)
{
    const std::size_t set = set_index(idx);
    for (std::size_t way = 0; way < ways; ++way) {
        auto& entry = table[set * ways + way];
        if (entry.valid)
            SaturatingCounter::dec(entry.u, std::uint8_t{0});
    }
}

void
tage_cmp::touch(idx_t idx, std::size_t way)
{
    auto& ranks = plru_table[set_index(idx)];
    if (way >= ranks.size())
        throw std::out_of_range("tage_cmp PLRU way is out of range");

    const std::uint8_t old_rank = ranks[way];
    const std::uint8_t max_rank =
        static_cast<std::uint8_t>(std::min<std::size_t>(ways - 1, 255));

    for (std::size_t i = 0; i < ranks.size(); ++i) {
        if (i != way && ranks[i] > old_rank)
            --ranks[i];
    }
    ranks[way] = max_rank;
}

std::uint8_t
tage_cmp::plru_value(idx_t idx, std::size_t way) const
{
    const auto& ranks = plru_table[set_index(idx)];
    if (way >= ranks.size())
        throw std::out_of_range("tage_cmp PLRU way is out of range");
    return ranks[way];
}

std::size_t
tage_cmp::way_count() const
{
    return ways;
}
