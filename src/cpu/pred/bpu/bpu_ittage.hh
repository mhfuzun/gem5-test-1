#pragma once

#include <cstddef>
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
    ittage_response_t lookup(bpu_addr_t pc);
    void clear_speculation();
    void squash_checkpoint(int checkpoint_id, bool include_self);
    bool commit(bpu_addr_t pc, bpu_addr_t target, bool taken);
    std::size_t checkpoint_count() const;

  private:
    struct entry_t
    {
        bool valid = false;
        std::uint64_t tag = 0;
        bpu_addr_t target = 0;
        std::uint8_t ctr = 0;
    };

    struct checkpoint_t
    {
        int id = -1;
        bpu_addr_t pc = 0;
        std::uint64_t ghr = 0;
        bool hit = false;
        bpu_addr_t target = 0;
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

    static constexpr std::size_t max_checkpoint_count = 1024;

    std::uint64_t ghr_mask() const;
    int index(bpu_addr_t pc) const;
    int index(bpu_addr_t pc, std::uint64_t history) const;
    std::uint64_t tag(bpu_addr_t pc) const;
    std::uint64_t tag(bpu_addr_t pc, std::uint64_t history) const;
    void update_history(bool taken);
    checkpoint_t* find_checkpoint(bpu_addr_t pc);
    void erase_checkpoint_id(int checkpoint_id);
    void record_target(bpu_addr_t pc, bpu_addr_t target,
                       std::uint64_t history);
    void trim_checkpoints();
};
