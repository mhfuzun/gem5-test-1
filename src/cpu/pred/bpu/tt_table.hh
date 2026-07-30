#pragma once

#include <cstdint>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class tt_table
{
    public:
        tt_table(tt_cfg cfg);
        bpu_addr_t generate_tag(bpu_addr_t pc) const;
        int get_bank_index(bpu_addr_t pc) const;
        int generate_index(bpu_addr_t pc) const;
        tt_bank_response_t lookup(bpu_addr_t pc);
        void insert_or_update(bpu_addr_t pc, bpu_addr_t target);
        void insert_or_update(bpu_addr_t pc, bpu_addr_t target,
                              bpu_addr_t tag_pc);
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
