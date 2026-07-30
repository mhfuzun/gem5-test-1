#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "bpu_structs.hh"
#include "cpu/pred/myTagePred/include/tage.h"

class bpu_tage
{
  public:
    bpu_tage();

    void reset();
    tage_response_t predict(const std::vector<tage_lookup_slot_t>& slots);
    std::vector<int> keep_path(const tage_response_t& response,
                               int stop_offset_2b,
                               bool has_stop);
    void discard_response(const tage_response_t& response);
    void clear_speculation();
    void squash_checkpoint(int checkpoint_id, bool include_self);
    bool commit(bpu_addr_t pc, bool taken);
    bool commit_checkpoint(int checkpoint_id, bpu_addr_t pc, bool taken);
    std::size_t checkpoint_count() const;

  private:
    struct checkpoint_t
    {
        int id = -1;
        bpu_addr_t pc = 0;
        bool prediction = false;
        tage::TAGE_State state;
    };

    tage_cfg_t cfg;
    std::unique_ptr<tage> predictor;
    std::deque<checkpoint_t> checkpoints;
    int next_checkpoint_id = 0;

    static constexpr std::size_t max_checkpoint_count = 4096;

    static tage_cfg_t make_default_cfg();
    checkpoint_t* find_checkpoint(bpu_addr_t pc);
    checkpoint_t* find_checkpoint_id(int checkpoint_id);
    void erase_checkpoint_id(int checkpoint_id);
    void trim_checkpoints();
};
