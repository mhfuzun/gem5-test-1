#pragma once

#include "bpu_structs.hh"
#include "btb.hh"
#include "ftq.hh"
#include "tt_table.hh"
#include "ubtb.hh"

class bpu
{
    public:
        bpu(btb_cfg btb_cfg, ubtb_cfg ubtb_cfg, tt_cfg tt_cfg,
            ftq_cfg ftq_cfg = {});

        void reset();
        void set_base_addr(int pc);
        bpu_cycle_output_t tick(const bpu_cycle_input_t& input);
        ubtb_response_t run_bpu1(int pc);
        btb_response_t run_bpu2(int pc, tage_response_t tage_response,
                                tt_bank_response_t tt_response,
                                ras_response_t ras_response);
        bpu_redirect_t run_bpu3(int pc, const btb_response_t& bpu2_response,
                                ittage_response_t ittage_response);
        void consume_ftq(int two_byte_count);
        int get_fetch_span() const;
        const ftq_entry_t* get_ftq_front() const;

        void update_ubtb(int pc, const ubtb_entry_t& entry);
        void update_btb(int pc, const btb_entry_t& entry);
        void update_tt(int pc, int target);

    private:
        int base_addr = 0;
        int cfi_addr = 0;
        int pending_next_cfi_span_2b = 0;
        int fetch_block_size_2b = 1;

        ubtb bpu_ubtb;
        btb bpu_btb;
        tt_table bpu_tt;
        ftq bpu_ftq;
        // ras
        // tage
        // ittage

        int compute_cfi_addr() const;
        int bank_align_span_2b(int span_2b) const;
        int cfi_inst_size_2b(const bpu_sign_t& sign) const;
        int fetch_span_through_taken_2b(const bpu_sign_t& sign) const;
        void advance_from_prediction(bool taken, int target,
                                     int next_cfi_span_2b);
        void advance_fallthrough(int next_cfi_span_2b);
};
