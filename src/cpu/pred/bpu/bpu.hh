#pragma once

#include <vector>

#include "bpu_ittage.hh"
#include "bpu_structs.hh"
#include "bpu_tage.hh"
#include "btb.hh"
#include "ftq.hh"
#include "tt_table.hh"
#include "ubtb.hh"

class bpu
{
    public:
        bpu(btb_cfg btb_cfg, ubtb_cfg ubtb_cfg, tt_cfg tt_cfg,
            ftq_cfg ftq_cfg = {}, bool enable_tage = false,
            bool enable_ras = false, bool enable_ittage = false);

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
        bool ftq_ready() const;
        bool ftq_empty() const;
        bool ftq_full() const;
        void recover(int pc, int speculative_id = -1,
                     bool include_self = true);

        void update_ubtb(int pc, const ubtb_entry_t& entry);
        void update_btb(int pc, const btb_entry_t& entry);
        void update_tt(int pc, int target);
        void commit(const bpu_commit_update_t& update);
        void retire_speculative_through(int speculative_id);
        void squash_speculative_after(int speculative_id);
        void squash_speculative_from(int speculative_id);
        const std::vector<bpu_speculative_node_t>& get_speculative_nodes()
            const;

    private:
        int base_addr = 0;
        int cfi_addr = 0;
        int pending_next_cfi_span_2b = 0;
        int fetch_block_size_2b = 1;
        int bank_size_2b = 1;
        bool bpu1_opens_new_ftq_entry = true;
        bool tage_enabled = false;
        bool ras_enabled = false;
        bool ittage_enabled = false;

        ubtb bpu_ubtb;
        btb bpu_btb;
        tt_table bpu_tt;
        ftq bpu_ftq;
        bpu_tage bpu_tage_predictor;
        bpu_ittage bpu_ittage_predictor;
        std::vector<bpu_speculative_node_t> speculative_nodes;
        std::vector<int> ras_stack;
        int next_speculative_id = 0;
        // tage
        // ittage

        int compute_cfi_addr() const;
        int bank_align_span_2b(int span_2b) const;
        int cfi_inst_size_2b(const bpu_sign_t& sign) const;
        int cfi_offset_from_base_2b(const bpu_sign_t& sign) const;
        int fetch_span_through_taken_2b(const bpu_sign_t& sign) const;
        bpu_sign_t make_ftq_base_sign(const bpu_sign_t& sign) const;
        void advance_from_prediction(bool taken, int target,
                                     int next_cfi_span_2b);
        void advance_fallthrough(int next_cfi_span_2b);
        int create_bpu2_speculative_node(int pc,
                                         const btb_response_t& response,
                                         const std::vector<int>&
                                             tage_checkpoint_ids,
                                         int ittage_checkpoint_id);
        bpu_speculative_node_t* find_speculative_node(int speculative_id);
        void mark_bpu3_resolved(int speculative_id);
        void mark_ittage_checkpoint(int speculative_id, int checkpoint_id);
        void mark_ubtb_fill(int speculative_id, int pc);
        ras_response_t make_ras_response(ras_response_t response) const;
        void update_ras_from_prediction(int pc,
                                        const btb_response_t& response);
        void squash_speculative_nodes(int speculative_id, bool include_self);
};
