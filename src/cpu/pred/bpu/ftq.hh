#pragma once

#include <deque>
#include <vector>

#include "bpu_structs.hh"

class ftq
{
    public:
        ftq(ftq_cfg cfg);
        ftq_entry_t* add_entry(bpu_addr_t base_addr, int fetch_span_2b);
        void add_fetch_span(ftq_entry_t& entry, int fetch_span_2b);
        void add_bpu2pushes(ftq_entry_t& entry,
                            std::vector<bpu_sign_t> cfi_sign_vector,
                            bpu_sign_t cfi_taken_sign);
        void add_bpu3pushes(ftq_entry_t& entry, bpu_addr_t target);
        void set_jalr_fail(ftq_entry_t& entry, bool jalr_fail);
        int get_fetch_span_2b() const;
        void consume(int two_byte_count);
        ftq_entry_t* front();
        const ftq_entry_t* front() const;
        ftq_entry_t* back();
        const ftq_entry_t* back() const;
        bool ready() const;
        bool empty() const;
        bool full() const;
        int size() const;
        void clear();

    private:
        ftq_cfg cfg;
        std::deque<ftq_entry_t> ftq_entries;

        void retire_consumed_fronts();
};
