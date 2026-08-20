#pragma once

#include <cstddef>
#include <vector>

#include "abtb.hh"
#include "bpu_base.hh"
#include "bpu_ittage.hh"
#include "bpu_structs.hh"
#include "bpu_tage.hh"
#include "btb.hh"
#include "ftq.hh"
#include "sbtb.hh"
#include "tt_table.hh"

class bpu_v2 : public bpu_base
{
  public:
    bpu_v2(btb_cfg btb_cfg, sbtb_cfg sbtb_cfg = {}, tt_cfg tt_cfg = {},
           ftq_cfg ftq_cfg = {}, bool enable_tage = false,
           bool enable_ras = false, bool enable_ittage = false,
           abtb_cfg abtb_cfg = {});

    void reset();
    void set_base_addr(bpu_addr_t pc);
    bpu_cycle_output_t tick(const bpu_cycle_input_t& input);
    ubtb_response_t run_abtb(bpu_addr_t pc);
    btb_response_t run_sbtb(bpu_addr_t pc);
    btb_response_t run_bpu3(bpu_addr_t pc, const bpu_cycle_input_t& input,
                            bpu_redirect_t& redirect,
                            tage_response_t& tage_response,
                            ittage_response_t& ittage_response);

    void consume_ftq(int two_byte_count);
    int get_fetch_span() const;
    const ftq_entry_t* get_ftq_front() const;
    bool ftq_ready() const;
    bool ftq_empty() const;
    bool ftq_full() const;
    void recover(bpu_addr_t pc, int speculative_id = -1,
                 bool include_self = true);

    void update_abtb(bpu_addr_t pc, const btb_response_t& response);
    void update_sbtb(bpu_addr_t pc, const btb_response_t& response,
                     bool taken);
    void update_btb(bpu_addr_t pc, const btb_entry_t& entry);
    void update_tt(bpu_addr_t pc, bpu_addr_t target);
    void commit(const bpu_commit_update_t& update);
    void retire_speculative_through(int speculative_id);
    void squash_speculative_after(int speculative_id);
    void squash_speculative_from(int speculative_id);
    const std::vector<bpu_speculative_node_t>& get_speculative_nodes() const;
    std::size_t speculative_node_count() const;
    std::size_t ras_depth() const;
    std::size_t tage_checkpoint_count() const;
    std::size_t ittage_checkpoint_count() const;

  private:
    struct remembered_btb_response_t
    {
        bool valid = false;
        int speculative_id = -1;
        bpu_addr_t lookup_pc = 0;
        bpu_addr_t cfi_pc = 0;
        btb_response_t response;
    };

    bpu_addr_t base_addr = 0;
    bpu_addr_t cfi_addr = 0;
    int pending_next_cfi_span_2b = 0;
    int fetch_block_size_2b = 1;
    int bank_size_2b = 1;
    bool bpu1_opens_new_ftq_entry = true;
    bool tage_enabled = false;
    bool ras_enabled = false;
    bool ittage_enabled = false;

    abtb bpu_abtb;
    sbtb bpu_sbtb;
    btb bpu_btb;
    tt_table bpu_tt;
    ftq bpu_ftq;
    bpu_tage bpu_tage_predictor;
    bpu_ittage bpu_ittage_predictor;
    std::vector<remembered_btb_response_t> remembered_btb_responses;
    std::vector<bpu_speculative_node_t> speculative_nodes;
    std::vector<bpu_addr_t> ras_stack;
    int next_speculative_id = 0;

    static constexpr std::size_t max_speculative_nodes = 512;
    static constexpr std::size_t max_ras_depth = 64;
    static constexpr std::size_t max_remembered_btb_responses = 512;

    static abtb_cfg make_default_abtb_cfg(const btb_cfg& btb_cfg,
                                          abtb_cfg abtb_cfg);
    static sbtb_cfg make_default_sbtb_cfg(const btb_cfg& btb_cfg,
                                          sbtb_cfg sbtb_cfg);

    bpu_addr_t compute_cfi_addr() const;
    int bank_align_span_2b(int span_2b) const;
    int span_from_next_cfi_addr(bpu_addr_t span_base,
                                bpu_addr_t next_cfi_addr,
                                int fallback_span_2b) const;
    bpu_addr_t next_cfi_addr_from_span(bpu_addr_t span_base,
                                       int span_2b) const;
    int cfi_inst_size_2b(const bpu_sign_t& sign) const;
    int cfi_offset_from_base_2b(int pending_span_2b,
                                const bpu_sign_t& sign) const;
    int fetch_span_through_taken_2b(int pending_span_2b,
                                    const bpu_sign_t& sign) const;
    bpu_sign_t make_ftq_base_sign(int pending_span_2b,
                                  const bpu_sign_t& sign) const;
    void advance_from_prediction(bool taken, bpu_addr_t target,
                                 int next_cfi_span_2b,
                                 bpu_addr_t next_cfi_addr = 0);
    void advance_fallthrough(int next_cfi_span_2b,
                             bpu_addr_t next_cfi_addr = 0);
    int create_speculative_node(bpu_addr_t pc,
                                const btb_response_t& response,
                                const std::vector<int>&
                                    tage_checkpoint_ids = {},
                                int ittage_checkpoint_id = -1);
    bpu_speculative_node_t* find_speculative_node(int speculative_id);
    void mark_bpu3_resolved(int speculative_id);
    void mark_ittage_checkpoint(int speculative_id, int checkpoint_id);
    ras_response_t make_ras_response(ras_response_t response) const;
    void update_ras_from_prediction(bpu_addr_t pc,
                                    const btb_response_t& response);
    void squash_speculative_nodes(int speculative_id, bool include_self);
    void trim_speculative_nodes();
    void push_ras(bpu_addr_t return_pc);

    ubtb_response_t make_abtb_response(const btb_response_t& response) const;
    void update_ftq_from_fast_response(const btb_response_t& response,
                                       int speculative_id);
    void update_ftq_from_btb_response(const btb_response_t& response);
    bool same_prediction(const btb_response_t& lhs,
                         const btb_response_t& rhs) const;
    bpu_redirect_t make_correction_redirect(
        bpu_addr_t lookup_pc, const btb_response_t& fast_response,
        const btb_response_t& response) const;
    void remember_btb_response(bpu_addr_t lookup_pc,
                               const btb_response_t& response);
    btb_response_t take_remembered_btb_response(
        int speculative_id, bpu_addr_t lookup_pc, bpu_addr_t cfi_pc);
    bpu_addr_t commit_lookup_pc(const bpu_commit_update_t& update) const;
    bpu_addr_t commit_cfi_pc(const bpu_commit_update_t& update) const;
    const btb_entry_record_t* find_commit_record(
        const bpu_commit_update_t& update) const;
    btb_response_t make_response_from_commit(
        const bpu_commit_update_t& update) const;
    void update_sbtb_from_commit(const bpu_commit_update_t& update);
    void update_abtb_from_commit(const bpu_commit_update_t& update,
                                 const btb_commit_result_t& btb_result);
};
