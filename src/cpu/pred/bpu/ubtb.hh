#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "bpu_structs.hh"

class ubtb
{
    public:
        ubtb(ubtb_cfg cfg);

        int generate_tag(int pc) const;
        ubtb_response_t predict(int pc);
        void insert_or_update(int pc, const ubtb_entry_t& entry);
        void invalidate(int pc);

    private:
        ubtb_cfg cfg;
        std::vector<ubtb_entry_t> ubtb_entries;
        std::unordered_map<int, std::size_t> tag_index;
        std::size_t next_replace = 0;

        ubtb_entry_t* lookup(int pc);
        const ubtb_entry_t* lookup(int pc) const;
        void rebuild_index();
};
