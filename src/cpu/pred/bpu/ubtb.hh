#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class ubtb
{
    public:
        ubtb(ubtb_cfg cfg);

        bpu_addr_t generate_tag(bpu_addr_t pc) const;
        ubtb_response_t predict(bpu_addr_t pc);
        void insert_or_update(bpu_addr_t pc, const ubtb_entry_t& entry);
        void invalidate(bpu_addr_t pc);

    private:
        ubtb_cfg cfg;
        std::vector<ubtb_entry_t> ubtb_entries;
        std::unordered_map<bpu_addr_t, std::size_t> tag_index;
        plru replacement;
        std::vector<std::uint8_t> replacement_state;

        ubtb_entry_t* lookup(bpu_addr_t pc);
        const ubtb_entry_t* lookup(bpu_addr_t pc) const;
        void rebuild_index();
};
