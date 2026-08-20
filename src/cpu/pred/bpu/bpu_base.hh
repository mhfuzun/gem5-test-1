#pragma once

#include <cstddef>
#include <vector>

#include "bpu_structs.hh"

class bpu_base
{
  public:
    virtual ~bpu_base() = default;

    virtual void reset() = 0;
    virtual void set_base_addr(bpu_addr_t pc) = 0;
    virtual bpu_cycle_output_t tick(const bpu_cycle_input_t& input) = 0;
    virtual void consume_ftq(int two_byte_count) = 0;
    virtual int get_fetch_span() const = 0;
    virtual const ftq_entry_t* get_ftq_front() const = 0;
    virtual bool ftq_ready() const = 0;
    virtual bool ftq_empty() const = 0;
    virtual bool ftq_full() const = 0;
    virtual void recover(bpu_addr_t pc, int speculative_id = -1,
                         bool include_self = true) = 0;

    virtual void commit(const bpu_commit_update_t& update) = 0;
    virtual void retire_speculative_through(int speculative_id) = 0;
    virtual void squash_speculative_after(int speculative_id) = 0;
    virtual void squash_speculative_from(int speculative_id) = 0;
    virtual const std::vector<bpu_speculative_node_t>&
        get_speculative_nodes() const = 0;
    virtual std::size_t speculative_node_count() const = 0;
    virtual std::size_t ras_depth() const = 0;
    virtual std::size_t tage_checkpoint_count() const = 0;
    virtual std::size_t ittage_checkpoint_count() const = 0;
};
