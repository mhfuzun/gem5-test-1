#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class sbtb
{
public:
    struct sbtb_entry_record_t
    {
        bool valid = false;
        int ctr = 0;
        bpu_addr_t target_addr = 0;
        bpu_addr_t next_cfi_addr = 0;
        int next_cfi_span_2b = 0;
        bpu_sign_t cfi_entry;
    };

    struct sbtb_entry_t
    {
        bool valid = false;
        bpu_tag_t tag = 0;
        sbtb_entry_record_t e1;
        sbtb_entry_record_t e2;
    };

    sbtb(sbtb_cfg cfg = {});

    int get_bank_index(bpu_addr_t addr) const;
    int generate_index(bpu_addr_t addr) const;
    bpu_tag_t generate_tag(bpu_addr_t addr) const;

    btb_response_t predict(bpu_addr_t cfi_addr);
    void insert_or_update(bpu_addr_t cfi_addr, const sbtb_entry_t& new_entry);
    void update(bpu_addr_t cfi_addr, bpu_addr_t target_addr,
                const bpu_sign_t& cfi_entry, bool taken,
                int next_cfi_span_2b = 0,
                bpu_addr_t next_cfi_addr = 0);
    void update(bpu_addr_t cfi_addr, const btb_response_t& response,
                bool taken);
    void invalidate(bpu_addr_t cfi_addr);
    void clear();

private:
    sbtb_cfg cfg;
    // [bank][set][way]
    std::vector<std::vector<std::vector<sbtb_entry_t>>> table;
    // [bank][set]: tag -> way
    std::vector<std::vector<std::unordered_map<bpu_tag_t, std::size_t>>>
        tag_index;
    // [bank][set]: invalid ways available without scanning the set
    std::vector<std::vector<std::vector<std::size_t>>> free_ways;
    plru replacement;
    // [bank][set]
    std::vector<std::vector<std::vector<std::uint8_t>>> replacement_states;

    int get_bank_2b_count() const;
    int get_bank_local_offset_2b(bpu_addr_t addr) const;
    bool is_taken(const sbtb_entry_record_t& record) const;
    bool is_after_lookup(const sbtb_entry_record_t& record,
                         int bank_offset,
                         int lookup_bank_offset_2b) const;
    bool same_cfi(const sbtb_entry_record_t& record,
                  const bpu_sign_t& sign) const;
    sbtb_entry_t* lookup_entry(int bank, int set, bpu_tag_t tag);
    const sbtb_entry_t* lookup_entry(int bank, int set, bpu_tag_t tag) const;
    void rebuild_index();
};
