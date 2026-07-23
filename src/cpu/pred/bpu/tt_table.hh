#pragma once

#include <vector>

#include "bpu_structs.hh"

class tt_table
{
    public:
        tt_table(tt_cfg cfg);
        int generate_tag(int pc) const;
        int get_bank_index(int pc) const;
        int generate_index(int pc) const;
        tt_bank_response_t lookup(int pc) const;
        void insert_or_update(int pc, int target);

    private:
        tt_cfg cfg;
        std::vector<std::vector<std::vector<tt_entry_t>>> tt;
};
