#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bpu_structs.hh"
#include "plru.hh"

class btb
{
    public:
        btb(btb_cfg cfg);

        int get_bank_index(bpu_addr_t pc) const;
        int generate_index(bpu_addr_t pc) const;
        bpu_addr_t generate_tag(bpu_addr_t pc) const;

        void predict(
            btb_response_t& response,
            bpu_addr_t pc,
            tage_response_t tage_response,
            tt_bank_response_t tt_response,
            ras_response_t ras_response
        );
        btb_response_t predict(
            bpu_addr_t pc,
            tage_response_t tage_response,
            tt_bank_response_t tt_response,
            ras_response_t ras_response
        );
        std::vector<tage_lookup_slot_t> lookup_tage_slots(
            bpu_addr_t pc) const;
        void insert_or_update(bpu_addr_t pc, const btb_entry_t& entry);
        btb_commit_result_t commit(const btb_commit_update_t& update);
        ubtb_entry_t make_ubtb_entry(bpu_addr_t cfi_addr,
                                     const btb_response_t& response) const;

    private:
        btb_cfg cfg;

        // [way][bank][set]
        std::vector<std::vector<std::vector<btb_entry_t>>> btb_table;
        plru replacement;
        // [bank][set]
        std::vector<std::vector<std::vector<std::uint8_t>>>
            replacement_states;

        struct btb_lookup_record_t
        {
            const btb_entry_record_t* record = nullptr;
            int fetch_block_offset_2b = 0;
            std::size_t way = 0;
            std::size_t set = 0;
            std::size_t tt_bank_slot = 0;
            std::size_t tage_slot = 0;
        };

        struct btb_lookup_bank_t
        {
            int bank = 0;
            std::size_t bank_slot = 0;
            std::vector<btb_lookup_record_t> records;
        };

        std::vector<btb_lookup_bank_t> lookup(bpu_addr_t pc) const;
        int get_bank_2b_count() const;
        bpu_sign_t make_fetch_block_sign(const bpu_sign_t& bank_sign,
                                         int fetch_block_offset_2b) const;
        void insert_or_update(bpu_addr_t pc, const btb_entry_t& entry,
                              bpu_addr_t tag_pc);
        bool is_direct_fillable(const btb_entry_record_t& entry) const;
        bool is_strongly_taken(const btb_entry_record_t& entry) const;
        btb_entry_record_t* find_branch_record(bpu_addr_t pc,
                                               const bpu_sign_t& branch_sign);
        btb_commit_result_t update_branch_ctr(bpu_addr_t pc,
                                              const bpu_sign_t& branch_sign,
                                              bool taken);
};
