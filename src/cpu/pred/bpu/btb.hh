#pragma once

#include <vector>

#include "bpu_structs.hh"

class btb
{
    public:
        btb(btb_cfg cfg);

        int get_bank_index(int pc) const;
        int generate_index(int pc) const;
        int generate_tag(int pc) const;

        void predict(
            btb_response_t& response,
            int pc,
            tage_response_t tage_response,
            tt_bank_response_t tt_response,
            ras_response_t ras_response
        );
        btb_response_t predict(
            int pc,
            tage_response_t tage_response,
            tt_bank_response_t tt_response,
            ras_response_t ras_response
        );
        void insert_or_update(int pc, const btb_entry_t& entry);

    private:
        btb_cfg cfg;

        // [way][bank][set]
        std::vector<std::vector<std::vector<btb_entry_t>>> btb_table;

        struct btb_lookup_bank_t
        {
            int bank = 0;
            std::vector<const btb_entry_record_t*> records;
        };

        std::vector<btb_lookup_bank_t> lookup(int pc) const;
};
