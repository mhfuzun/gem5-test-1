#pragma once

#include <cstdint>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class tt_table
{
    public:
        tt_table(tt_cfg cfg);
        int generate_tag(int pc) const;
        int get_bank_index(int pc) const;
        int generate_index(int pc) const;
        tt_bank_response_t lookup(int pc);
        void insert_or_update(int pc, int target);
        void insert_or_update(int pc, int target, int tag_pc);
        void commit(const tt_commit_update_t& update);

    private:
        tt_cfg cfg;
        std::vector<std::vector<std::vector<tt_entry_t>>> tt;
        plru replacement;
        // [bank][set]
        std::vector<std::vector<std::vector<std::uint8_t>>>
            replacement_states;
        int get_bank_2b_count() const;
};
