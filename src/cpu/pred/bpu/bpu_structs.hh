#pragma once

#include <vector>

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
    int offset = 0;
    cfi_type_t type = CFI_NULL;
    bool compressed = false;
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
    // Taken path and fallthrough path CFI-probe distances. Both are measured
    // in 2-byte units and should already point to a bank-aligned fetch block.
    int taken_next_cfi_span_2b = 0;
    int fallthrough_next_cfi_span_2b = 0;
};

struct btb_entry_t
{
    bool valid = false;
    int tag = 0;
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
};

struct tage_response_t
{
    bool predictions[2] = {false, false};
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
};

struct ftq_entry_t
{
    bool valid = false;
    int base_addr = 0;
    // How many 2-byte chunks IFU should fetch starting from base_addr.
    int fetch_span_2b = 0;
    int target = 0;
    // Bank-aligned distance from target/base to the next predicted CFI block.
    int next_cfi_span_2b = 0;
    bool jalr_fail = false;
    bpu_sign_t cfi_taken_sign;
    std::vector<bpu_sign_t> cfi_sign_vector;
};

struct bpu_cycle_input_t
{
    bool base_valid = false;
    int base_addr = 0;
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
    bool ftq_pushed = false;
    bool ftq_updated = false;
};
