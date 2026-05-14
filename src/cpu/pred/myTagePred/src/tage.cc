#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // rand
#include <ctime>   // time
#include <stdexcept>

#include "cpu/pred/myTagePred/include/generic.h"
#include "cpu/pred/myTagePred/include/lfsr.h"
#include "cpu/pred/myTagePred/include/tage.h"

namespace {
inline std::uint32_t mid_hi_for_width(int ctr_width) {
  if (ctr_width <= 0)
    return 0;
  return (1u << static_cast<std::uint32_t>(ctr_width - 1));
}

inline bool pred_from_ctr(std::uint32_t ctr, int ctr_width) {
  return ctr >= mid_hi_for_width(ctr_width);
}

inline bool ctr_is_weak(std::uint32_t ctr, int ctr_width) {
  const std::uint32_t mid_hi = mid_hi_for_width(ctr_width);
  const std::uint32_t mid_lo = (mid_hi == 0) ? 0u : (mid_hi - 1);
  return (ctr == mid_hi) || (ctr == mid_lo);
}
} // namespace

bool tage::provider_exists(const TAGE_State &state) {
  return state.provider_idx != static_cast<std::size_t>(-1);
}

bool tage::altpred_exists(const TAGE_State &state) {
  return state.altpred_idx != static_cast<std::size_t>(-1);
}

void tage::initStatisticsStorage() {
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

tage::tage(const tage_cfg_t &cfg)
    : tage_cfg(cfg), component_list(), ghistory_reg(cfg.ghistory_length),
      pchistory(cfg.pchistory_length),
      bimodal_predictor(static_cast<std::size_t>(cfg.bimodal_depth)),
      lfsr_reg((cfg.random_type == LFSR && cfg.lfsr_width > 0) ? cfg.lfsr_width
                                                               : 1,
               cfg.lfsr_seed) {
  if (cfg.bimodal_depth <= 0)
    throw std::invalid_argument("tage_cfg.bimodal_depth must be > 0");
  if (cfg.ghistory_length < 0)
    throw std::invalid_argument("tage_cfg.ghistory_length must be >= 0");
  if (cfg.comp_count < 0)
    throw std::invalid_argument("tage_cfg.comp_count must be >= 0");
  if (cfg.random_type == LFSR && (cfg.lfsr_width <= 0 || cfg.lfsr_width > 64))
    throw std::invalid_argument(
        "tage_cfg.lfsr_width must be in [1, 64] when random_type is LFSR");
  if (static_cast<std::size_t>(cfg.comp_count) > cfg.table_cfg.size())
    throw std::invalid_argument("tage_cfg.table_cfg size < comp_count");

  component_list.reserve(static_cast<std::size_t>(cfg.comp_count));
  for (int i = 0; i < cfg.comp_count; ++i) {
    const auto &tc = cfg.table_cfg[static_cast<std::size_t>(i)];
    if (tc.depth <= 0)
      throw std::invalid_argument("table_cfg.depth must be > 0");
    if (tc.ctr_width <= 0)
      throw std::invalid_argument("table_cfg.ctr_width must be > 0");
    if (tc.tag_width <= 0)
      throw std::invalid_argument("table_cfg.tag_width must be > 0");
    if (tc.usefull_width <= 0)
      throw std::invalid_argument("table_cfg.usefull_width must be > 0");
    if (tc.history_width < 0)
      throw std::invalid_argument("table_cfg.history_width must be >= 0");
    component_list.emplace_back(cfg.table_cfg[static_cast<std::size_t>(i)]);
  }

  if (!cfg.table_cfg.empty())
    usefull_reset_flipState = std::max(0, cfg.table_cfg[0].usefull_width - 1);

  initStatisticsStorage();
}

void tage::snapshotState(TAGE_State &state) const {
  state = TAGE_State();
  state.ghistSnapshot = ghistory_reg;
  state.pchistSnapshot = pchistory;
  state.usefull_reset_ctr = usefull_reset_ctr;
  state.usefull_reset_flipState = usefull_reset_flipState;
  state.comp_idx.assign(static_cast<std::size_t>(tage_cfg.comp_count), 0);
  state.comp_tag.assign(static_cast<std::size_t>(tage_cfg.comp_count), 0);
  state.comp_row.assign(static_cast<std::size_t>(tage_cfg.comp_count),
                        nullptr);
}

// fetch anında buyruk geldiğinde çalışır
bool tage::getPrediction(addr_t addr, TAGE_State &state) {
  snapshotState(state);

  state.bimodal_ctr = bimodal_predictor.getPrediction(addr);
  state.bimpred_res = pred_from_ctr(state.bimodal_ctr, 2);

  for (int i = 0; i < tage_cfg.comp_count; ++i) {
    std::uint64_t idx_hash = 0;
    std::uint64_t tag_hash = 0;

    const auto &tc = tage_cfg.table_cfg[static_cast<std::size_t>(i)];
    const int idx_width =
        idxWidthForDepth(static_cast<std::uint64_t>(tc.depth));
    const std::size_t history_width =
        (tc.history_width > 0) ? static_cast<std::size_t>(tc.history_width)
                               : 0u;
    const std::size_t path_start =
        (tc.pchistory_start > 0) ? static_cast<std::size_t>(tc.pchistory_start)
                                 : 0u;
    const std::size_t path_width =
        (tc.pchistory_width > 0) ? static_cast<std::size_t>(tc.pchistory_width)
                                 : 0u;

    // hash = folded_history_hash ^ pc_bits
    idx_hash ^= getIdx(
        addr, (tage_cfg.pc_hash_start_for_idx + tage_cfg.comp_count - i),
        tage_cfg.pc_hash_width_for_idx);
    idx_hash ^= static_cast<std::uint64_t>(
        ghistory_reg.hash_foldedHistory(history_width, idx_width));
    if (tage_cfg.pchistory_length != 0 && path_width > 0) {
      const int path_idx_width =
          std::max(1, std::min(idx_width, tc.pchistory_width));
      idx_hash ^= static_cast<std::uint64_t>(pchistory.hash_foldedHistory(
          path_start, path_width, path_idx_width));
    }
    if (idx_width < 64)
      idx_hash &=
          ((std::uint64_t{1} << static_cast<std::uint64_t>(idx_width)) - 1);

    tag_hash ^= getIdx(addr, tage_cfg.pc_hash_start_for_tag,
                       tage_cfg.pc_hash_width_for_tag);
    tag_hash ^= static_cast<std::uint64_t>(
        ghistory_reg.hash_foldedHistory(history_width, tc.tag_width));
    if (tage_cfg.pchistory_length != 0 && path_width > 0) {
      const int path_tag_width =
          std::max(1, std::min(tc.tag_width, tc.pchistory_width));
      tag_hash ^= static_cast<std::uint64_t>(pchistory.hash_foldedHistory(
          path_start, path_width, path_tag_width));
    }
    if (tc.tag_width < 64)
      tag_hash &=
          ((std::uint64_t{1} << static_cast<std::uint64_t>(tc.tag_width)) - 1);

    state.comp_idx[static_cast<std::size_t>(i)] = static_cast<idx_t>(idx_hash);
    state.comp_tag[static_cast<std::size_t>(i)] = tag_hash;
    auto &row = component_list[static_cast<std::size_t>(i)].row(
        static_cast<idx_t>(idx_hash));
    state.comp_row[static_cast<std::size_t>(i)] = &row;
  }

  comp_idx = state.comp_idx;
  comp_tag = state.comp_tag;

  // Provider: choose the longest-history (highest index) matching component.
  for (int i = tage_cfg.comp_count - 1; i >= 0; --i) {
    const std::size_t si = static_cast<std::size_t>(i);
    const auto &tc = tage_cfg.table_cfg[si];
    const int tag_width = tc.tag_width;
    std::uint64_t mask =
        ((std::uint64_t{1} << static_cast<std::uint64_t>(tag_width)) - 1);
    if (state.comp_row[si] &&
        (state.comp_tag[si] & mask) == (state.comp_row[si]->tag & mask)) {
      state.provider_idx = static_cast<std::size_t>(i);
      break;
    }
  }

  // Altpred: next best match below provider.
  if (provider_exists(state) && state.provider_idx > 0) {
    for (int i = static_cast<int>(state.provider_idx) - 1; i >= 0; --i) {
      const std::size_t si = static_cast<std::size_t>(i);
      const auto &tc = tage_cfg.table_cfg[si];
      const int tag_width = tc.tag_width;
      std::uint64_t mask =
          ((std::uint64_t{1} << static_cast<std::uint64_t>(tag_width)) - 1);
      if (state.comp_row[si] &&
          (state.comp_tag[si] & mask) == (state.comp_row[si]->tag & mask)) {
        state.altpred_idx = static_cast<std::size_t>(i);
        break;
      }
    }
  }

  if (!provider_exists(state)) {
    state.predType = PRED_BIMODAL;
    stats.tableUse.bimMapCount++;
    state.pred = state.bimpred_res;
    return state.pred;
  }

  const auto &ptc = tage_cfg.table_cfg[state.provider_idx];
  const std::uint32_t pctr = state.comp_row[state.provider_idx]->ctr;
  state.providerpred_res = pred_from_ctr(pctr, ptc.ctr_width);

  if (!ctr_is_weak(pctr, ptc.ctr_width)) {
    state.predType = PRED_PROVIDER;
    stats.tableUse.providerMapCount[state.provider_idx]++;
    state.pred = state.providerpred_res;
    return state.pred;
  }

  if (altpred_exists(state)) {
    state.predType = PRED_ALTPRED;
    const auto &atc = tage_cfg.table_cfg[state.altpred_idx];
    const std::uint32_t actr = state.comp_row[state.altpred_idx]->ctr;
    stats.tableUse.altPredMapCount[state.altpred_idx]++;
    state.altpred_res = pred_from_ctr(actr, atc.ctr_width);
    state.pred = state.altpred_res;
    return state.pred;
  }

  state.predType = PRED_BIMODAL;
  stats.tableUse.bimMapCount++;
  state.pred = state.bimpred_res;
  return state.pred;
}

bool tage::getPrediction(addr_t addr) {
  lastStateValid = true;
  return getPrediction(addr, lastState);
}

const statistics_t &tage::getStats() const { return stats; }

void tage::resetStats() {
  initStatisticsStorage();
  lastStateValid = false;
}

// tahmin yanlış yapıldığında pchistory ve branch history kurtarılır.
void tage::resetState(const TAGE_State &state) {
  ghistory_reg = state.ghistSnapshot;
  pchistory = state.pchistSnapshot;
  usefull_reset_ctr = state.usefull_reset_ctr;
  usefull_reset_flipState = state.usefull_reset_flipState;
}

// yürütme sonrası history güncellenir.
void tage::updateHist(addr_t addr, bool outcome, const TAGE_State &state) {
  (void)state;
  // Update global history.
  ghistory_reg.push(outcome);

  if (tage_cfg.pchistory_length != 0)
    pchistory.push((((addr >> 2) ^ (addr >> 5) ^ (addr >> 11)) & 1u) != 0);
}

void tage::update(addr_t addr, bool outcome) {
  if (!lastStateValid) {
    (void)getPrediction(addr, lastState);
  }

  update(addr, outcome, lastState);
  updateHist(addr, outcome, lastState);
  lastStateValid = false;
}

// commit anında tablolar güncellenir.
void tage::update(addr_t addr, bool outcome, const TAGE_State &state) {
  // Allocate new entries on mispredict (simple policy).
  const int comp_count = tage_cfg.comp_count;
  if (comp_count > 0) {
    const int start = (!provider_exists(state))
                          ? 0
                          : static_cast<int>(state.provider_idx) + 1;
    bool need_alloc = (state.pred != outcome);

    if (provider_exists(state)) {
      if (!need_alloc)
        stats.noAllocationBecauseOfHit++;
      if (need_alloc && !(start < comp_count) &&
          (state.comp_row[state.provider_idx]->u == 0)) {
        stats.noAllocationButProviderZero++;
      }
    }

    if (need_alloc && !(start < comp_count))
      stats.noAllocationBecauseOfproviderHigh++;

    if (need_alloc) {
      int freeCompCount = 0;
      std::vector<int> freeCompList(static_cast<std::size_t>(comp_count), 0);
      int freeComp_idx = -1;

      for (int i = comp_count - 1; i >= start; --i) {
        if (state.comp_row[static_cast<std::size_t>(i)]->u == 0) {
          freeCompList[i] = 1;
          freeCompCount++;

          if (i > freeComp_idx)
            freeComp_idx = i;
        } else {
          freeCompList[i] = 0;
        }
      }

      if (tage_cfg.allocate_randomplacement)
        if (freeCompCount > 0) {
          int target;
          if (tage_cfg.random_type == SIMRAND) {
            target = rand() % freeCompCount;
          } else {
            lfsr_reg.nextRand();
            target =
                static_cast<int>(lfsr_reg.foldedhash(getCompIdxWidth()) %
                                 static_cast<std::uint64_t>(freeCompCount));
          }

          int count = 0;
          int i = (tage_cfg.allocate_longerThanProvider) ? start : 0;
          for (; i < comp_count; ++i) {
            if (freeCompList[i] == 1) {
              if (count == target) {
                freeComp_idx = i;
                break;
              }
              count++;
            }
          }
        }

      if (freeCompCount > 0) {
        const auto free_idx = static_cast<std::size_t>(freeComp_idx);
        const auto &tc = tage_cfg.table_cfg[free_idx];

        const std::uint32_t mid_hi = mid_hi_for_width(tc.ctr_width);
        const std::uint32_t mid_lo = (mid_hi == 0) ? 0u : (mid_hi - 1);

        state.comp_row[free_idx]->u = 0;
        state.comp_row[free_idx]->ctr =
            static_cast<std::uint8_t>(outcome ? mid_hi : mid_lo);
        state.comp_row[free_idx]->tag = state.comp_tag[free_idx];

        stats.tableUse.allocationMapCount[free_idx]++;
      } else {
        for (int i = start; i < comp_count; ++i)
          SaturatingCounter::dec(
              state.comp_row[static_cast<std::size_t>(i)]->u,
              std::uint8_t{0});
        stats.noAllocationBecauseOfnoFree++;
      }
    }
  }

  // Update usefulness (u) when provider/alt disagree: reward the correct one.
  if (provider_exists(state)) {
    const auto &ptc = tage_cfg.table_cfg[state.provider_idx];

    bool alt_pred = state.bimpred_res;
    if (altpred_exists(state)) {
      alt_pred = state.altpred_res;
    }

    if (state.providerpred_res != alt_pred && ptc.usefull_width > 0) {
      if (state.providerpred_res == outcome /*&& alt_pred != outcome*/)
        SaturatingCounter::update_bits(state.comp_row[state.provider_idx]->u,
                                       true, ptc.usefull_width);
      else if (state.providerpred_res != outcome /*&& alt_pred == outcome*/)
        SaturatingCounter::update_bits(state.comp_row[state.provider_idx]->u,
                                       false, ptc.usefull_width);
    }
  }

  // Update prediction counter.
  if (provider_exists(state)) {
    SaturatingCounter::update_bits(
        state.comp_row[state.provider_idx]->ctr, outcome,
        tage_cfg.table_cfg[state.provider_idx].ctr_width);
  } else {
    auto bimodal_ctr = state.bimodal_ctr;
    SaturatingCounter::update_bits(bimodal_ctr, outcome, 2);
    bimodal_predictor.update(addr, bimodal_ctr);
  }

  // usefull reset
  if (tage_cfg.periodicreset && !component_list.empty() &&
      tage_cfg.periodicreset_branchperiod > 0) {
    usefull_reset_ctr++;
    if (usefull_reset_ctr ==
        static_cast<std::uint64_t>(tage_cfg.periodicreset_branchperiod)) {
      for (auto &comp : component_list) {
        comp.reset_usefull_bits(usefull_reset_flipState);
      }

      usefull_reset_ctr = 0;
      usefull_reset_flipState--;
      if (usefull_reset_flipState < 0) {
        usefull_reset_flipState =
            std::max(0, tage_cfg.table_cfg[0].usefull_width - 1);
      }
    }
  }

  if (tage_cfg.random_type == LFSR && tage_cfg.lfsr_misprediction_update)
    if (outcome != state.pred) {
      addr_t hash = addr ^ (addr >> 3) ^ (addr << 5);
      lfsr_reg.updateRand(static_cast<std::uint64_t>(hash));
    }

  // statistics
  stats.totalBranchCount++;
  if (outcome)
    stats.takenBranchCount++;
  if (outcome != state.pred)
    stats.missPredictionCount++;

  if (outcome == state.bimpred_res)
    stats.tableUse.bimHitCount++;
  if (outcome == state.bimpred_res && state.predType == PRED_BIMODAL) {
    stats.tableUse.bimHitCountWhenBimUsed++;
  }

  if (altpred_exists(state)) {
    if (outcome == state.altpred_res)
      stats.tableUse.altpredHitCount++;
    if (outcome == state.altpred_res && state.predType == PRED_ALTPRED) {
      stats.tableUse.altpredHitCountWhenAltpredUsed++;
    }

    if (outcome == state.altpred_res && state.predType == PRED_ALTPRED) {
      stats.tableUse.altPredHitCountList[state.altpred_idx]++;
    }
  }

  if (provider_exists(state)) {
    if (outcome == state.providerpred_res)
      stats.tableUse.providerHitCount++;
    if (outcome == state.providerpred_res && state.predType == PRED_PROVIDER) {
      stats.tableUse.providerHitCountWhenProviderUsed++;
    }

    if (outcome == state.providerpred_res && state.predType == PRED_PROVIDER) {
      stats.tableUse.providerHitCountList[state.provider_idx]++;
    }
  }
}

void tage::DisplayStatistics(void) { stats.display(); }

//

tage_cmp::tage_cmp(table_cfg_t cfg)
    : table_cfg(cfg),
      table(cfg.depth > 0 ? static_cast<std::size_t>(cfg.depth) : 0) {}

void tage_cmp::reset_usefull_bits(int bit_index) {
  if (bit_index < 0 || bit_index >= 8)
    return;
  for (auto &row : table) {
    row.u &= static_cast<std::uint8_t>(
        ~(std::uint8_t{1} << static_cast<unsigned>(bit_index)));
  }
}

const tage_comp_t &tage_cmp::row(idx_t idx) const {
  const std::size_t i =
      static_cast<std::size_t>(idx % static_cast<idx_t>(table.size()));
  return table[i];
}

tage_comp_t &tage_cmp::row(idx_t idx) {
  const std::size_t i =
      static_cast<std::size_t>(idx % static_cast<idx_t>(table.size()));
  return table[i];
}
