#pragma once

#include <vector>

struct bpu_cfg
{
    static constexpr int fetch_block_2b_count = 16;
    static constexpr int fetch_block_halfword_count = fetch_block_2b_count;
    static constexpr int fetch_block_halfWorld_cnt = fetch_block_2b_count;
    static constexpr int btb_branch_ctr_max = 3;
    static constexpr int btb_branch_ctr_strong_taken = 3;
};

struct ubtb_cfg
{
    int way_count = 0;
    int tag_width = 0;
    int tag_pc_shift = 0;
};

struct btb_cfg
{
    int way_count = 0;
    int set_count = 0;
    int bank_count = 0;
    int tag_width = 0;
    int tag_pc_shift = 0;
};

struct tt_cfg
{
    int way_count = 0;
    int set_count = 0;
    int bank_count = 0;
    int tag_width = 0;
    int tag_pc_shift = 0;
};

struct ftq_cfg
{
    int depth = 0;
};

enum cfi_type_t
{
    CFI_NULL,
    CFI_BRA,
    CFI_JAL,
    CFI_JALR_CALL,
    CFI_JALR_RET
};

struct bpu_sign_t
{
    /**
     * btb için bank başlangıcına kadar olandır.
     * ubtb için ise cfi_addr (fetch block) başlangıcına kadar olandır.
     * cfi_tracer bank hizalı olacak şekilde btb içine yazar.
     * btb ise hazır veri haline getirip ubtb içine yazar.
     */
    // Yani aynı CFI iki farklı formatta taşınır: BTB record içindeki offset
    // bank-local, BPU/FTQ/uBTB response içindeki offset ise fetch block-local.
    // Fetch span hesabı yalnızca fetch block-local offset ile yapılmalıdır.
    int offset = 0;
    cfi_type_t type = CFI_NULL;
    bool compressed = false;
    bool is_call = false;
};

struct ubtb_entry_t
{
    bool valid = false;
    int tag = 0;
    int target = 0;
    bpu_sign_t cfi_sign;
    // Bank-aligned distance, in 2-byte units, from the redirect base to the
    // next fetch block that should be probed for CFI.
    int next_cfi_span_2b = 0;
};

struct btb_entry_record_t
{
    bool valid = false;
    int target = 0;
    bpu_sign_t cfi_sign;
    int branch_ctr = 0;
    // Taken path and fallthrough path CFI-probe distances. Both are measured
    // in 2-byte units and should already point to a bank-aligned fetch block.
    int taken_next_cfi_span_2b = 0;
    int fallthrough_next_cfi_span_2b = 0;
};

struct btb_entry_t
{
    bool valid = false;
    int tag = 0;
    // Logical fetch-block offset inside the banked tag group. This is only a
    // small halfword offset, not a full PC; it keeps records from different
    // shifted bases from being merged into the same BTB entry.
    int base_offset_2b = 0;
    btb_entry_record_t e1;
    btb_entry_record_t e2;
};

struct tt_entry_t
{
    bool valid = false;
    int tag = 0;
    int target = 0;
};

struct ubtb_response_t
{
    bool valid = false;
    bool taken = false;
    int target = 0;
    int next_cfi_span_2b = 0;
    bpu_sign_t sign;
};

struct btb_response_t
{
    bool valid = false;
    bool taken = false;
    int target = 0;
    int next_cfi_span_2b = 0;
    bpu_sign_t sign;
    bool tt_hit = false;
    bool ras_valid = false;
    bool tage_used = false;
    bool ubtb_fillable = false;
    bool branch_strongly_taken = false;
    int speculative_id = -1;
    int tage_slot = -1;
    int tage_checkpoint_id = -1;
};

struct tage_lookup_slot_t
{
    bool valid = false;
    int pc = 0;
    bpu_sign_t sign;
};

struct tage_response_t
{
    bool valid = false;
    std::vector<tage_lookup_slot_t> slots;
    std::vector<bool> slot_valid;
    std::vector<bool> predictions;
    std::vector<int> checkpoint_ids;
};

struct ras_response_t
{
    bool valid = false;
    int target = 0;
};

struct tt_response_t
{
    bool hit = false;
    int target = 0;
};

struct tt_bank_response_t
{
    std::vector<tt_response_t> banks;
};

struct ittage_response_t
{
    bool hit = false;
    int target = 0;
    int next_cfi_span_2b = 0;
    int checkpoint_id = -1;
};

struct ftq_entry_t
{
    bool valid = false;
    int base_addr = 0;
    // How many 2-byte chunks IFU should fetch starting from base_addr.
    int fetch_span_2b = 0;
    // How many 2-byte chunks have already been accepted by IFU/cache.
    int consumed_span_2b = 0;
    int target = 0;
    // Bank-aligned distance from target/base to the next predicted CFI block.
    int next_cfi_span_2b = 0;
    bool jalr_fail = false;
    int speculative_id = -1;
    bpu_sign_t cfi_taken_sign;
    std::vector<bpu_sign_t> cfi_sign_vector;
};

struct bpu_cycle_input_t
{
    bool base_valid = false;
    int base_addr = 0;
    bool use_tage = false;
    bool use_ras = false;
    bool use_ittage = false;
    tage_response_t tage_response;
    ras_response_t ras_response;
    tt_bank_response_t tt_response;
    ittage_response_t ittage_response;
};

struct bpu_redirect_t
{
    bool valid = false;
    int target = 0;
    int next_cfi_span_2b = 0;
    bpu_sign_t sign;
};

struct bpu_cycle_output_t
{
    ubtb_response_t bpu1;
    btb_response_t bpu2;
    bpu_redirect_t redirect;
    tage_response_t tage_response;
    ittage_response_t ittage_response;
    int lookup_cfi_addr = 0;
    int bpu1_old_fetch_span_2b = 0;
    int bpu1_new_fetch_span_2b = 0;
    int bpu1_added_fetch_span_2b = 0;
    int bpu1_next_cfi_addr = 0;
    int bpu2_old_fetch_span_2b = 0;
    int bpu2_new_fetch_span_2b = 0;
    int bpu2_next_cfi_addr = 0;
    int bpu3_old_fetch_span_2b = 0;
    int bpu3_new_fetch_span_2b = 0;
    int bpu3_next_cfi_addr = 0;
    bool bpu3_redirect = false;
    bool ftq_pushed = false;
    bool ftq_updated = false;
    bool ubtb_filled = false;
};

struct btb_commit_update_t
{
    bool valid = false;
    bool insert_entry = false;
    // Physical bank PC used to select the BTB bank/set that will receive this
    // update. For CFI tracer updates this is fetch_block + bank_slot stride.
    int pc = 0;
    // Logical lookup PC used for the BTB tag and for bank-slot walkback.
    // When valid, branch_sign.offset is fetch-block-local, not bank-local.
    bool lookup_pc_valid = false;
    int lookup_pc = 0;
    bool ubtb_pc_valid = false;
    int ubtb_pc = 0;
    btb_entry_t entry;
    bool update_branch_ctr = false;
    bpu_sign_t branch_sign;
    bool branch_taken = false;
};

struct btb_commit_result_t
{
    bool branch_ctr_updated = false;
    bool branch_strongly_taken = false;
};

struct tt_commit_update_t
{
    bool valid = false;
    // Physical bank PC. This keeps TT in the same bank lane as the BTB JALR
    // record that requested the target.
    int pc = 0;
    // Logical lookup PC used for TT tag generation, matching the BTB lookup
    // base for that fetch block.
    bool lookup_pc_valid = false;
    int lookup_pc = 0;
    int target = 0;
};

struct bpu_commit_update_t
{
    btb_commit_update_t btb_update;
    tt_commit_update_t tt_update;
    int speculative_id = -1;
};

struct bpu_speculative_node_t
{
    bool valid = false;
    int id = -1;
    int cfi_addr = 0;
    btb_response_t bpu2_response;
    bool resolved_by_bpu3 = false;
    bool flushed = false;
    bool ubtb_fill_valid = false;
    int ubtb_fill_pc = 0;
    std::vector<int> tage_checkpoint_ids;
    int ittage_checkpoint_id = -1;
    std::vector<int> ras_snapshot;
};
