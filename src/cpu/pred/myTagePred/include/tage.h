#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bimodal.h"
#include "cfg.h"
#include "ghistory.h"
#include "lfsr.h"

struct tage_comp_t
{
  std::uint64_t tag = 0;
  std::uint8_t ctr = 0;
  std::uint8_t u = 0;
};

class tage_cmp
{
public:
  explicit tage_cmp(table_cfg_t table_cfg);
  ~tage_cmp() = default;

  const tage_comp_t &row(idx_t idx) const;
  tage_comp_t &row(idx_t idx);
  void reset_usefull_bits(int bit_index);

private:
  [[maybe_unused]] table_cfg_t table_cfg;
  std::vector<tage_comp_t> table;
};

class tage
{
public:
  struct TAGE_State
  {
    // State captured at prediction time for speculative repair
    // and final table update().
    ghistory ghistSnapshot;
    ghistory pchistSnapshot;

    std::vector<idx_t> comp_idx;
    std::vector<std::uint64_t> comp_tag;
    std::vector<tage_comp_t *> comp_row;
    ctr2_t bimodal_ctr = 2;

    std::size_t provider_idx = static_cast<std::size_t>(-1);
    std::size_t altpred_idx = static_cast<std::size_t>(-1);

    int bimpred_res = 0;
    int altpred_res = 0;
    int providerpred_res = 0;

    int pred = 0;
    pred_type_t predType = PRED_NONE;

    uint64_t usefull_reset_ctr = 0;
    int usefull_reset_flipState = 0;
  };

  explicit tage(const tage_cfg_t &tage_cfg);
  ~tage() = default;

  bool getPrediction(addr_t addr, TAGE_State &state);
  bool getPrediction(addr_t addr);
  void snapshotState(TAGE_State &state) const;
  void resetState(const TAGE_State &state);
  void updateHist(addr_t addr, bool outcome, const TAGE_State &state);
  void update(addr_t addr, bool outcome, const TAGE_State &state);
  void update(addr_t addr, bool outcome);
  const statistics_t &getStats() const;
  void resetStats();
  void DisplayStatistics(void);

private:
  friend struct tage_test_access;

  tage_cfg_t tage_cfg;
  std::vector<tage_cmp> component_list;

  ghistory ghistory_reg;
  ghistory pchistory;
  bimodal bimodal_predictor;
  lfsr lfsr_reg;
  statistics_t stats;
  TAGE_State lastState;
  bool lastStateValid = false;

  uint64_t usefull_reset_ctr = 0;
  int usefull_reset_flipState = 0;

  std::vector<idx_t> comp_idx;
  std::vector<std::uint64_t> comp_tag;

  void initStatisticsStorage();

  static bool provider_exists(const TAGE_State &state);
  static bool altpred_exists(const TAGE_State &state);

  int getCompIdxWidth() const {
    return std::max(1, idxWidthForDepth(static_cast<std::uint64_t>(
                           std::max(tage_cfg.comp_count, 1))));
  }
};
