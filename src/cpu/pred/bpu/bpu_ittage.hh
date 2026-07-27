#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class bpu_ittage
{
  public:
    bpu_ittage();

    void reset();
    ittage_response_t lookup(int pc);
    void clear_speculation();
    void squash_checkpoint(int checkpoint_id, bool include_self);
    bool commit(int pc, int target, bool taken);

  private:
    struct entry_t
    {
        bool valid = false;
        std::uint64_t tag = 0;
        int target = 0;
        std::uint8_t ctr = 0;
    };

    struct checkpoint_t
    {
        int id = -1;
        int pc = 0;
        std::uint64_t ghr = 0;
        bool hit = false;
        int target = 0;
    };

    static constexpr int set_count = 512;
    static constexpr int way_count = 2;
    static constexpr int history_bits = 16;
    static constexpr int tag_bits = 16;
    static constexpr int inst_shift = 1;

    std::vector<std::vector<entry_t>> table;
    plru replacement;
    std::vector<std::vector<std::uint8_t>> replacement_states;
    std::deque<checkpoint_t> checkpoints;
    std::uint64_t ghr = 0;
    int next_checkpoint_id = 0;

    std::uint64_t ghr_mask() const;
    int index(int pc) const;
    int index(int pc, std::uint64_t history) const;
    std::uint64_t tag(int pc) const;
    std::uint64_t tag(int pc, std::uint64_t history) const;
    void update_history(bool taken);
    checkpoint_t* find_checkpoint(int pc);
    void erase_checkpoint_id(int checkpoint_id);
    void record_target(int pc, int target, std::uint64_t history);
};
