#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bimodal.h"
#include "cfg.h"
#include "ghistory.h"
#include "ghistory_register.h"
#include "lfsr.h"

struct tage_comp_t
{
    std::uint64_t tag = 0;
    std::uint32_t offset = 0;
    std::uint8_t ctr = 0;
    std::uint8_t u = 0;
    bool valid = false;
};

class tage_cmp
{
  public:
    tage_cmp(table_cfg_t table_cfg, int entry_per_set);
    ~tage_cmp() = default;

    const tage_comp_t& row(idx_t idx) const;
    tage_comp_t& row(idx_t idx);
    const tage_comp_t& row(idx_t idx, std::size_t way) const;
    tage_comp_t& row(idx_t idx, std::size_t way);
    std::size_t victim_way(idx_t idx) const;
    bool useful_allocation_way(idx_t idx, std::size_t& way) const;
    void decay_useful_bits(idx_t idx);
    void touch(idx_t idx, std::size_t way);
    std::uint8_t plru_value(idx_t idx, std::size_t way) const;
    std::size_t way_count() const;
    void reset_usefull_bits(int bit_index);

  private:
    table_cfg_t table_cfg;
    std::size_t ways = 1;
    std::vector<tage_comp_t> table;
    std::vector<std::vector<std::uint8_t>> plru_table;

    std::size_t set_index(idx_t idx) const;
    std::size_t flat_index(idx_t idx, std::size_t way) const;
};

class tage
{
  public:
    struct TAGE_State
    {
        ghistory ghistSnapshot;
        ghistory pchistSnapshot;
        std::vector<ghistory_register> idxHistoryHashSnapshot;
        std::vector<ghistory_register> tagHistoryHashSnapshot;

        std::vector<idx_t> comp_idx;
        std::vector<std::uint64_t> comp_tag;
        std::vector<tage_comp_t*> comp_row;
        std::vector<std::size_t> comp_way;
        addr_t index_addr = 0;
        addr_t lookup_addr = 0;
        addr_t tag_addr = 0;
        addr_t bimodal_addr = 0;
        addr_t bimodal_lookup_addr = 0;
        ctr2_t bimodal_ctr = 2;

        std::size_t provider_idx = static_cast<std::size_t>(-1);
        std::size_t altpred_idx = static_cast<std::size_t>(-1);
        std::size_t provider_way = static_cast<std::size_t>(-1);
        std::size_t altpred_way = static_cast<std::size_t>(-1);

        int bimpred_res = 0;
        int altpred_res = 0;
        int providerpred_res = 0;

        int pred = 0;
        pred_type_t predType = PRED_NONE;
        bool provider_pseudo_new_alloc = false;
        bool altpred_is_bimodal = false;
        bool use_alt_on_na_active = false;
        std::size_t use_alt_on_na_idx = 0;

        uint64_t usefull_reset_ctr = 0;
        int usefull_reset_flipState = 0;
    };

    explicit tage(const tage_cfg_t& tage_cfg);
    ~tage() = default;

    bool getPrediction(addr_t addr, TAGE_State& state);
    bool getPrediction(addr_t index_addr, addr_t lookup_addr,
                       const TAGE_State& history_state,
                       TAGE_State& prediction_state);
    bool getPrediction(addr_t addr);
    void snapshotState(TAGE_State& state) const;
    void resetState(const TAGE_State& state);
    void updateHist(addr_t addr, bool outcome, const TAGE_State& state);
    void update(addr_t addr, bool outcome, const TAGE_State& state);
    void update(addr_t addr, bool outcome);
    const statistics_t& getStats() const;
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

    // State captured at prediction time for update().
    std::vector<idx_t> comp_idx;
    std::vector<std::uint64_t> comp_tag;
    std::vector<tage_comp_t*> comp_row;
    std::vector<std::size_t> comp_way;
    ctr2_t bimodal_ctr = 2;
    std::vector<std::int8_t> useAltPredForNewlyAllocated;

    std::size_t provider_idx = -1;
    std::size_t altpred_idx = -1;
    std::size_t provider_way = -1;
    std::size_t altpred_way = -1;

    int bimpred_res = 0;
    int altpred_res = 0;
    int providerpred_res = 0;

    int pred = 0;
    pred_type_t predType = PRED_NONE;
    bool provider_pseudo_new_alloc = false;
    bool altpred_is_bimodal = false;
    bool use_alt_on_na_active = false;
    std::size_t use_alt_on_na_idx = 0;

    statistics_t stats;
    TAGE_State lastState;
    bool lastStateValid = false;

    uint64_t usefull_reset_ctr = 0;
    int usefull_reset_flipState = 0;

    std::vector<ghistory_register> idx_history_hash;
    std::vector<ghistory_register> tag_history_hash;

    void initStatisticsStorage();
    static bool provider_exists(const TAGE_State& state);
    static bool altpred_exists(const TAGE_State& state);
    std::uint32_t pc_offset(addr_t addr) const;
    std::uint64_t global_history_hash(const TAGE_State& state,
                                      std::size_t table_idx, int width) const;
    void update_history_hashes(bool outcome);
    std::vector<int> allocation_candidates(const TAGE_State& state) const;
    std::size_t use_alt_on_na_index(addr_t addr) const;
    bool use_alt_on_na_prefers_alt(std::size_t idx) const;
    void update_use_alt_on_na(const TAGE_State& state, bool alt_correct);
    void record_prediction_use(const TAGE_State& state);
    void record_prediction_hits(const TAGE_State& state, bool outcome);

    int getCompIdxWidth() const
    {
        return std::max(1, idxWidthForDepth(static_cast<std::uint64_t>(
                               std::max(tage_cfg.comp_count, 1))));
    }
};
