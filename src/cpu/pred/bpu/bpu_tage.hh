#pragma once

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
    void clear_speculation();
    void squash_checkpoint(int checkpoint_id, bool include_self);
    bool commit(int pc, bool taken);
    bool commit_checkpoint(int checkpoint_id, int pc, bool taken);

  private:
    struct checkpoint_t
    {
        int id = -1;
        int pc = 0;
        bool prediction = false;
        tage::TAGE_State state;
    };

    tage_cfg_t cfg;
    std::unique_ptr<tage> predictor;
    std::deque<checkpoint_t> checkpoints;
    int next_checkpoint_id = 0;

    static tage_cfg_t make_default_cfg();
    checkpoint_t* find_checkpoint(int pc);
    checkpoint_t* find_checkpoint_id(int checkpoint_id);
    void erase_checkpoint_id(int checkpoint_id);
};
