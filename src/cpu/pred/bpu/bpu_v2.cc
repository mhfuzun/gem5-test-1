#include <algorithm>
#include <iterator>

#include "bpu_v2.hh"

abtb_cfg
bpu_v2::make_default_abtb_cfg(const btb_cfg& btb_cfg, abtb_cfg abtb_cfg)
{
    if (abtb_cfg.bank_count <= 0) {
        abtb_cfg.bank_count = 4;
    }
    if (abtb_cfg.set_count <= 0) {
        abtb_cfg.set_count = 2048;
    }
    if (abtb_cfg.tag_width <= 0) {
        abtb_cfg.tag_width = btb_cfg.tag_width;
    }
    if (abtb_cfg.tag_pc_shift <= 0) {
        abtb_cfg.tag_pc_shift = btb_cfg.tag_pc_shift;
    }
    return abtb_cfg;
}

sbtb_cfg
bpu_v2::make_default_sbtb_cfg(const btb_cfg& btb_cfg, sbtb_cfg sbtb_cfg)
{
    if (sbtb_cfg.bank_count <= 0) {
        sbtb_cfg.bank_count = btb_cfg.bank_count;
    }
    if (sbtb_cfg.set_count <= 0) {
        sbtb_cfg.set_count = btb_cfg.set_count;
    }
    if (sbtb_cfg.way_count <= 0) {
        sbtb_cfg.way_count = btb_cfg.way_count;
    }
    if (sbtb_cfg.tag_width <= 0) {
        sbtb_cfg.tag_width = btb_cfg.tag_width;
    }
    if (sbtb_cfg.tag_pc_shift <= 0) {
        sbtb_cfg.tag_pc_shift = btb_cfg.tag_pc_shift;
    }
    return sbtb_cfg;
}

bpu_v2::bpu_v2(btb_cfg btb_cfg, sbtb_cfg sbtb_cfg, tt_cfg tt_cfg,
               ftq_cfg ftq_cfg, bool enable_tage, bool enable_ras,
               bool enable_ittage, abtb_cfg abtb_cfg)
    : bpu_abtb(make_default_abtb_cfg(btb_cfg, abtb_cfg)),
      bpu_sbtb(make_default_sbtb_cfg(btb_cfg, sbtb_cfg)),
      bpu_btb(btb_cfg),
      bpu_tt(tt_cfg),
      bpu_ftq(ftq_cfg)
{
    fetch_block_size_2b = bpu_cfg::fetch_block_2b_count;
    bank_size_2b =
        std::max(1, fetch_block_size_2b / std::max(1, btb_cfg.bank_count));
    tage_enabled = enable_tage;
    ras_enabled = enable_ras;
    ittage_enabled = enable_ittage;
}

void
bpu_v2::reset()
{
    base_addr = 0;
    cfi_addr = 0;
    pending_next_cfi_span_2b = 0;
    bpu1_opens_new_ftq_entry = true;
    bpu_ftq.clear();
    bpu_sbtb.clear();
    remembered_btb_responses.clear();
    speculative_nodes.clear();
    ras_stack.clear();
    bpu_tage_predictor.reset();
    bpu_ittage_predictor.reset();
}

void
bpu_v2::set_base_addr(bpu_addr_t pc)
{
    base_addr = pc;
    pending_next_cfi_span_2b = 0;
    bpu1_opens_new_ftq_entry = true;
    cfi_addr = compute_cfi_addr();
}

bpu_cycle_output_t
bpu_v2::tick(const bpu_cycle_input_t& input)
{
    if (input.base_valid) {
        set_base_addr(input.base_addr);
    }

    cfi_addr = compute_cfi_addr();
    const bpu_addr_t lookup_cfi_addr = cfi_addr;
    bpu_cycle_output_t output;
    output.lookup_cfi_addr = lookup_cfi_addr;

    const int ftq_size_before = bpu_ftq.size();

    const int bpu1_old_size = bpu_ftq.size();
    const ftq_entry_t* bpu1_old_entry = bpu_ftq.back();
    output.bpu1_old_fetch_span_2b =
        bpu1_old_entry == nullptr ? 0 : bpu1_old_entry->fetch_span_2b;
    output.bpu1 = run_abtb(lookup_cfi_addr);
    const ftq_entry_t* bpu1_new_entry = bpu_ftq.back();
    output.bpu1_new_fetch_span_2b =
        bpu1_new_entry == nullptr ? 0 : bpu1_new_entry->fetch_span_2b;
    output.bpu1_old_fetch_span_2b =
        bpu_ftq.size() > bpu1_old_size ? 0 :
        output.bpu1_old_fetch_span_2b;
    output.bpu1_added_fetch_span_2b =
        std::max(0, output.bpu1_new_fetch_span_2b -
                    output.bpu1_old_fetch_span_2b);
    output.bpu1_next_cfi_addr =
        output.bpu1.valid && output.bpu1.taken ?
        (output.bpu1.next_cfi_addr != 0 ?
         output.bpu1.next_cfi_addr :
         next_cfi_addr_from_span(output.bpu1.target,
                                 output.bpu1.next_cfi_span_2b)) :
        base_addr + output.bpu1_new_fetch_span_2b * 2;

    const ftq_entry_t* bpu2_old_entry = bpu_ftq.back();
    output.bpu2_old_fetch_span_2b =
        bpu2_old_entry == nullptr ? 0 : bpu2_old_entry->fetch_span_2b;
    const btb_response_t sbtb_response = run_sbtb(lookup_cfi_addr);
    output.bpu2 = sbtb_response;
    const ftq_entry_t* bpu2_new_entry = bpu_ftq.back();
    output.bpu2_new_fetch_span_2b =
        bpu2_new_entry == nullptr ? 0 : bpu2_new_entry->fetch_span_2b;
    output.bpu2_next_cfi_addr =
        output.bpu2.valid && output.bpu2.taken ?
        (output.bpu2.next_cfi_addr != 0 ?
         output.bpu2.next_cfi_addr :
         next_cfi_addr_from_span(output.bpu2.target,
                                 output.bpu2.next_cfi_span_2b)) :
        output.bpu2.valid ?
            (output.bpu2.next_cfi_addr != 0 ?
             output.bpu2.next_cfi_addr :
             base_addr + output.bpu2_new_fetch_span_2b * 2) : 0;

    btb_response_t fast_response;
    if (sbtb_response.valid && sbtb_response.taken) {
        fast_response = sbtb_response;
    } else if (output.bpu1.valid && output.bpu1.taken) {
        fast_response.valid = true;
        fast_response.taken = true;
        fast_response.target = output.bpu1.target;
        fast_response.next_cfi_addr = output.bpu1.next_cfi_addr;
        fast_response.next_cfi_span_2b = output.bpu1.next_cfi_span_2b;
        fast_response.sign = output.bpu1.sign;
    }

    const ftq_entry_t* bpu3_old_entry = bpu_ftq.back();
    output.bpu3_old_fetch_span_2b =
        bpu3_old_entry == nullptr ? 0 : bpu3_old_entry->fetch_span_2b;
    bpu_redirect_t bpu3_redirect;
    tage_response_t bpu3_tage_response;
    ittage_response_t bpu3_ittage_response;
    const btb_response_t bpu3_response =
        run_bpu3(lookup_cfi_addr, input, bpu3_redirect,
                 bpu3_tage_response, bpu3_ittage_response);
    output.bpu3 = bpu3_response;
    output.tage_response = bpu3_tage_response;
    output.ittage_response = bpu3_ittage_response;
    const ftq_entry_t* bpu3_new_entry = bpu_ftq.back();
    output.bpu3_new_fetch_span_2b =
        bpu3_new_entry == nullptr ? 0 : bpu3_new_entry->fetch_span_2b;

    const bpu_redirect_t correction =
        make_correction_redirect(lookup_cfi_addr, fast_response,
                                 bpu3_response);
    if (bpu3_redirect.valid) {
        output.redirect = bpu3_redirect;
    } else if (correction.valid) {
        output.redirect = correction;
    } else if (fast_response.valid && fast_response.taken) {
        output.redirect.valid = true;
        output.redirect.target = fast_response.target;
        output.redirect.next_cfi_addr = fast_response.next_cfi_addr;
        output.redirect.next_cfi_span_2b =
            fast_response.next_cfi_span_2b;
        output.redirect.sign = fast_response.sign;
    }
    output.bpu3_redirect = bpu3_redirect.valid || correction.valid;
    output.bpu3_next_cfi_addr = output.redirect.valid ?
        (output.redirect.next_cfi_addr != 0 ?
         output.redirect.next_cfi_addr :
         next_cfi_addr_from_span(output.redirect.target,
                                 output.redirect.next_cfi_span_2b)) : 0;

    output.ftq_pushed = bpu_ftq.size() > ftq_size_before;
    output.ftq_updated =
        output.bpu2.valid || bpu3_response.valid || output.redirect.valid;

    if (output.redirect.valid) {
        advance_from_prediction(true, output.redirect.target,
                                output.redirect.next_cfi_span_2b,
                                output.redirect.next_cfi_addr);
    } else if (bpu3_response.valid &&
               ((bpu3_response.sign.type == CFI_JALR_CALL &&
                 !bpu3_response.tt_hit) ||
                (bpu3_response.sign.type == CFI_JALR_RET &&
                 !bpu3_response.ras_valid))) {
        cfi_addr = compute_cfi_addr();
    } else if (bpu3_response.valid) {
        advance_fallthrough(bpu3_response.next_cfi_span_2b,
                            bpu3_response.next_cfi_addr);
    } else {
        advance_fallthrough(output.bpu1.next_cfi_span_2b,
                            output.bpu1.next_cfi_addr);
    }

    bpu1_opens_new_ftq_entry =
        output.redirect.valid || (fast_response.valid && fast_response.taken);
    return output;
}

ubtb_response_t
bpu_v2::make_abtb_response(const btb_response_t& response) const
{
    ubtb_response_t abtb_response;
    abtb_response.valid = response.valid;
    abtb_response.taken = response.taken;
    abtb_response.target = response.target;
    abtb_response.next_cfi_addr = response.next_cfi_addr;
    abtb_response.next_cfi_span_2b = response.next_cfi_span_2b;
    abtb_response.sign = response.sign;
    return abtb_response;
}

ubtb_response_t
bpu_v2::run_abtb(bpu_addr_t pc)
{
    const btb_response_t abtb_response = bpu_abtb.predict(pc);
    const ubtb_response_t response = make_abtb_response(abtb_response);
    const int fetch_span_2b = response.valid && response.taken ?
        fetch_span_through_taken_2b(pending_next_cfi_span_2b, response.sign) :
        pending_next_cfi_span_2b + fetch_block_size_2b;

    ftq_entry_t* entry = nullptr;
    if (bpu1_opens_new_ftq_entry || bpu_ftq.back() == nullptr) {
        entry = bpu_ftq.add_entry(base_addr, fetch_span_2b);
    } else {
        entry = bpu_ftq.back();
        const int span_delta =
            std::max(0, fetch_span_2b - entry->fetch_span_2b);
        bpu_ftq.add_fetch_span(*entry, span_delta);
    }

    if (entry != nullptr) {
        entry->target = response.valid && response.taken ?
            response.target : 0;
        entry->next_cfi_addr = response.next_cfi_addr;
        entry->next_cfi_span_2b =
            span_from_next_cfi_addr(response.target,
                                    response.next_cfi_addr,
                                    response.next_cfi_span_2b);
        entry->cfi_taken_sign = response.valid && response.taken ?
            make_ftq_base_sign(pending_next_cfi_span_2b, response.sign) :
            bpu_sign_t{};
        if (response.valid && response.taken) {
            entry->abtb_taken_valid = true;
            entry->abtb_taken_sign =
                make_ftq_base_sign(pending_next_cfi_span_2b,
                                   response.sign);
            entry->abtb_target = response.target;
        }
    }

    return response;
}

btb_response_t
bpu_v2::run_sbtb(bpu_addr_t pc)
{
    btb_response_t response = bpu_sbtb.predict(pc);
    if (!response.valid) {
        return response;
    }

    response.speculative_id =
        create_speculative_node(pc, response, {}, -1);
    update_ras_from_prediction(pc, response);
    update_ftq_from_fast_response(response, response.speculative_id);
    return response;
}

btb_response_t
bpu_v2::run_bpu3(bpu_addr_t pc, const bpu_cycle_input_t& input,
                 bpu_redirect_t& redirect,
                 tage_response_t& tage_response,
                 ittage_response_t& ittage_response)
{
    tt_bank_response_t tt_response = input.tt_response;
    if (tt_response.banks.empty()) {
        tt_response = bpu_tt.lookup(pc);
    }

    tage_response = input.tage_response;
    if (input.use_tage && tage_enabled && !tage_response.valid) {
        tage_response =
            bpu_tage_predictor.predict(bpu_btb.lookup_tage_slots(pc));
    }

    ras_response_t effective_ras_response =
        make_ras_response(input.ras_response);
    btb_response_t btb_response =
        bpu_btb.predict(pc, tage_response, tt_response,
                        effective_ras_response);
    if (btb_response.valid) {
        const bool stops_at_response =
            btb_response.taken ||
            btb_response.sign.type == CFI_JAL ||
            btb_response.sign.type == CFI_JALR_CALL ||
            btb_response.sign.type == CFI_JALR_RET;
        const std::vector<int> tage_checkpoint_ids =
            tage_response.valid ?
            bpu_tage_predictor.keep_path(
                tage_response, btb_response.sign.offset, stops_at_response) :
            std::vector<int>{};
        btb_response.speculative_id =
            create_speculative_node(pc, btb_response,
                                    tage_checkpoint_ids, -1);
        update_ras_from_prediction(pc, btb_response);
    } else if (tage_response.valid) {
        bpu_tage_predictor.discard_response(tage_response);
    }

    update_ftq_from_btb_response(btb_response);
    mark_bpu3_resolved(btb_response.speculative_id);

    ftq_entry_t* entry = bpu_ftq.back();
    if (entry == nullptr || !btb_response.valid) {
        return btb_response;
    }

    if (btb_response.sign.type != CFI_JALR_CALL) {
        remember_btb_response(pc, btb_response);
        return btb_response;
    }

    ittage_response = input.ittage_response;
    if (input.use_ittage && ittage_enabled &&
        ittage_response.checkpoint_id < 0) {
        const bpu_addr_t jalr_pc =
            pc + static_cast<bpu_addr_t>(
                std::max(0, btb_response.sign.offset)) * 2;
        ittage_response = bpu_ittage_predictor.lookup(jalr_pc);
        mark_ittage_checkpoint(btb_response.speculative_id,
                               ittage_response.checkpoint_id);
    }

    if (ittage_response.hit) {
        const bool needs_late_redirect =
            !btb_response.tt_hit ||
            btb_response.target != ittage_response.target;
        entry->target = ittage_response.target;
        entry->next_cfi_addr = ittage_response.next_cfi_addr != 0 ?
            ittage_response.next_cfi_addr :
            next_cfi_addr_from_span(ittage_response.target,
                                    ittage_response.next_cfi_span_2b);
        entry->next_cfi_span_2b =
            span_from_next_cfi_addr(ittage_response.target,
                                    entry->next_cfi_addr,
                                    ittage_response.next_cfi_span_2b);
        entry->fetch_span_2b = std::max(
            fetch_span_through_taken_2b(pending_next_cfi_span_2b,
                                        btb_response.sign),
            entry->consumed_span_2b);
        entry->jalr_fail = false;
        entry->btb_prediction_valid = true;
        entry->btb_prediction_taken = true;
        entry->btb_prediction_sign =
            make_ftq_base_sign(pending_next_cfi_span_2b,
                               btb_response.sign);
        entry->btb_prediction_target = ittage_response.target;

        btb_response.taken = true;
        btb_response.target = ittage_response.target;
        btb_response.next_cfi_addr = entry->next_cfi_addr;
        btb_response.next_cfi_span_2b = entry->next_cfi_span_2b;
        btb_response.tt_hit = true;

        if (needs_late_redirect) {
            redirect.valid = true;
            redirect.target = ittage_response.target;
            redirect.next_cfi_addr = entry->next_cfi_addr;
            redirect.next_cfi_span_2b = entry->next_cfi_span_2b;
            redirect.sign = btb_response.sign;
            squash_speculative_after(btb_response.speculative_id);
        }

        remember_btb_response(pc, btb_response);
        return btb_response;
    }

    if (!btb_response.tt_hit) {
        bpu_ftq.set_jalr_fail(*entry, true);
    }

    remember_btb_response(pc, btb_response);
    return btb_response;
}

void
bpu_v2::update_ftq_from_fast_response(const btb_response_t& response,
                                      int speculative_id)
{
    ftq_entry_t* entry = bpu_ftq.back();
    if (entry == nullptr || !response.valid) {
        return;
    }

    const bool is_indirect =
        response.sign.type == CFI_JALR_CALL ||
        response.sign.type == CFI_JALR_RET;
    const bool stops_at_response = response.taken || is_indirect;

    if (stops_at_response) {
        entry->fetch_span_2b = std::max(
            fetch_span_through_taken_2b(pending_next_cfi_span_2b,
                                        response.sign),
            entry->consumed_span_2b);
    }

    entry->target = response.target;
    entry->next_cfi_addr = response.next_cfi_addr;
    entry->next_cfi_span_2b =
        span_from_next_cfi_addr(response.target, response.next_cfi_addr,
                                response.next_cfi_span_2b);
    entry->jalr_fail = is_indirect && !response.taken;
    entry->cfi_taken_sign = stops_at_response ?
        make_ftq_base_sign(pending_next_cfi_span_2b, response.sign) :
        bpu_sign_t{};
    entry->speculative_id = speculative_id;
    if (response.valid && response.taken) {
        entry->sbtb_taken_valid = true;
        entry->sbtb_taken_sign =
            make_ftq_base_sign(pending_next_cfi_span_2b, response.sign);
        entry->sbtb_target = response.target;
    }
}

void
bpu_v2::update_ftq_from_btb_response(const btb_response_t& response)
{
    ftq_entry_t* entry = bpu_ftq.back();
    if (entry == nullptr || !response.valid) {
        return;
    }

    const bpu_sign_t ftq_base_sign =
        make_ftq_base_sign(pending_next_cfi_span_2b, response.sign);
    const bool is_indirect =
        response.sign.type == CFI_JALR_CALL ||
        response.sign.type == CFI_JALR_RET;
    const bool stops_at_response = response.taken || is_indirect;
    const bool unresolved_indirect = is_indirect && !response.taken;

    if (stops_at_response) {
        entry->fetch_span_2b = std::max(
            fetch_span_through_taken_2b(pending_next_cfi_span_2b,
                                        response.sign),
            entry->consumed_span_2b);
    } else if (response.sign.type == CFI_BRA) {
        entry->fetch_span_2b = std::max(
            span_from_next_cfi_addr(
                base_addr, response.next_cfi_addr,
                pending_next_cfi_span_2b +
                    bank_align_span_2b(response.next_cfi_span_2b)),
            entry->consumed_span_2b);
    }

    entry->target = response.target;
    entry->next_cfi_addr = response.next_cfi_addr;
    entry->next_cfi_span_2b =
        span_from_next_cfi_addr(response.taken ? response.target : base_addr,
                                response.next_cfi_addr,
                                response.next_cfi_span_2b);
    entry->jalr_fail = unresolved_indirect;
    entry->cfi_taken_sign = stops_at_response ? ftq_base_sign : bpu_sign_t{};
    entry->speculative_id = response.speculative_id;
    entry->btb_prediction_valid = true;
    entry->btb_prediction_taken = response.taken;
    entry->btb_prediction_sign = ftq_base_sign;
    entry->btb_prediction_target = response.target;
    if (response.sign.type == CFI_BRA && response.tage_used) {
        entry->tage_prediction_valid = true;
        entry->tage_prediction_taken = response.taken;
        entry->tage_prediction_sign = ftq_base_sign;
    }
}

bool
bpu_v2::same_prediction(const btb_response_t& lhs,
                        const btb_response_t& rhs) const
{
    return lhs.valid == rhs.valid &&
        lhs.taken == rhs.taken &&
        lhs.target == rhs.target &&
        lhs.next_cfi_addr == rhs.next_cfi_addr &&
        lhs.next_cfi_span_2b == rhs.next_cfi_span_2b &&
        lhs.sign.offset == rhs.sign.offset &&
        lhs.sign.type == rhs.sign.type &&
        lhs.sign.compressed == rhs.sign.compressed &&
        lhs.sign.is_call == rhs.sign.is_call;
}

bpu_redirect_t
bpu_v2::make_correction_redirect(bpu_addr_t lookup_pc,
                                 const btb_response_t& fast_response,
                                 const btb_response_t& response) const
{
    bpu_redirect_t redirect;
    if (!response.valid || same_prediction(fast_response, response) ||
        (!fast_response.valid && !response.taken)) {
        return redirect;
    }

    redirect.valid = true;
    redirect.sign = response.sign;
    redirect.next_cfi_addr = response.next_cfi_addr;
    redirect.next_cfi_span_2b = response.next_cfi_span_2b;
    if (response.taken) {
        redirect.target = response.target;
    } else {
        redirect.target = lookup_pc +
            static_cast<bpu_addr_t>(
                std::max(0, response.sign.offset) +
                cfi_inst_size_2b(response.sign)) * 2;
    }

    return redirect;
}

void
bpu_v2::consume_ftq(int two_byte_count)
{
    bpu_ftq.consume(two_byte_count);
}

int
bpu_v2::get_fetch_span() const
{
    return bpu_ftq.get_fetch_span_2b();
}

const ftq_entry_t*
bpu_v2::get_ftq_front() const
{
    return bpu_ftq.front();
}

bool
bpu_v2::ftq_ready() const
{
    return bpu_ftq.ready();
}

bool
bpu_v2::ftq_empty() const
{
    return !bpu_ftq.ready();
}

bool
bpu_v2::ftq_full() const
{
    return bpu_ftq.full();
}

void
bpu_v2::recover(bpu_addr_t pc, int speculative_id, bool include_self)
{
    if (speculative_id >= 0) {
        squash_speculative_nodes(speculative_id, include_self);
    } else {
        speculative_nodes.clear();
        bpu_tage_predictor.clear_speculation();
        bpu_ittage_predictor.clear_speculation();
        ras_stack.clear();
    }

    bpu_ftq.clear();
    set_base_addr(pc);
}

void
bpu_v2::update_abtb(bpu_addr_t pc, const btb_response_t& response)
{
    bpu_abtb.insert_or_update(pc, response);
}

void
bpu_v2::update_sbtb(bpu_addr_t pc, const btb_response_t& response,
                    bool taken)
{
    bpu_sbtb.update(pc, response, taken);
}

void
bpu_v2::update_btb(bpu_addr_t pc, const btb_entry_t& entry)
{
    bpu_btb.insert_or_update(pc, entry);
}

void
bpu_v2::update_tt(bpu_addr_t pc, bpu_addr_t target)
{
    bpu_tt.insert_or_update(pc, target);
}

void
bpu_v2::update_sbtb_from_commit(const bpu_commit_update_t& update)
{
    if (!update.btb_update.valid) {
        return;
    }

    const bpu_addr_t cfi_pc = commit_cfi_pc(update);
    bpu_addr_t target = 0;
    bpu_addr_t next_cfi_addr = 0;
    int next_cfi_span_2b = 0;
    const btb_entry_record_t* record = find_commit_record(update);
    if (record != nullptr) {
        target = record->target;
        next_cfi_addr = record->taken_next_cfi_addr;
        next_cfi_span_2b = record->taken_next_cfi_span_2b;
    }

    if (update.tt_update.valid) {
        target = update.tt_update.target;
        next_cfi_addr = update.tt_update.target;
    }

    bpu_sbtb.update(cfi_pc, target, update.btb_update.branch_sign,
                    update.btb_update.branch_taken ||
                    update.btb_update.branch_sign.type == CFI_JAL,
                    next_cfi_span_2b, next_cfi_addr);
}

void
bpu_v2::commit(const bpu_commit_update_t& update)
{
    const btb_commit_result_t btb_result =
        bpu_btb.commit(update.btb_update);
    update_sbtb_from_commit(update);
    update_abtb_from_commit(update, btb_result);

    bpu_tt.commit(update.tt_update);

    const bpu_addr_t cfi_pc = commit_cfi_pc(update);
    if (tage_enabled && update.btb_update.valid &&
        update.btb_update.branch_sign.type == CFI_BRA) {
        int tage_checkpoint_id = -1;
        bpu_speculative_node_t* node =
            find_speculative_node(update.speculative_id);
        if (node != nullptr && node->bpu2_response.sign.type == CFI_BRA) {
            const bpu_addr_t node_cfi_pc =
                node->cfi_addr +
                std::max(0, node->bpu2_response.sign.offset) * 2;
            if (node_cfi_pc == cfi_pc) {
                tage_checkpoint_id =
                    node->bpu2_response.tage_checkpoint_id;
            }
        }

        bpu_tage_predictor.commit_checkpoint(
            tage_checkpoint_id, cfi_pc, update.btb_update.branch_taken);
    }

    if (ittage_enabled && update.tt_update.valid &&
        update.btb_update.branch_sign.type == CFI_JALR_CALL) {
        bpu_ittage_predictor.commit(cfi_pc, update.tt_update.target, true);
    }
}

void
bpu_v2::remember_btb_response(bpu_addr_t lookup_pc,
                              const btb_response_t& response)
{
    if (!response.valid) {
        return;
    }

    remembered_btb_response_t remembered;
    remembered.valid = true;
    remembered.speculative_id = response.speculative_id;
    remembered.lookup_pc = lookup_pc;
    remembered.cfi_pc =
        lookup_pc + static_cast<bpu_addr_t>(
            std::max(0, response.sign.offset)) * 2;
    remembered.response = response;
    remembered_btb_responses.push_back(remembered);
    while (remembered_btb_responses.size() >
           max_remembered_btb_responses) {
        remembered_btb_responses.erase(remembered_btb_responses.begin());
    }
}

btb_response_t
bpu_v2::take_remembered_btb_response(int speculative_id,
                                     bpu_addr_t lookup_pc,
                                     bpu_addr_t cfi_pc)
{
    for (auto it = remembered_btb_responses.rbegin();
         it != remembered_btb_responses.rend(); ++it) {
        if (!it->valid) {
            continue;
        }

        const bool speculative_match =
            speculative_id >= 0 && it->speculative_id == speculative_id;
        const bool pc_match =
            it->lookup_pc == lookup_pc && it->cfi_pc == cfi_pc;
        if (!speculative_match && !pc_match) {
            continue;
        }

        btb_response_t response = it->response;
        remembered_btb_responses.erase(std::next(it).base());
        return response;
    }

    return {};
}

bpu_addr_t
bpu_v2::commit_lookup_pc(const bpu_commit_update_t& update) const
{
    return update.btb_update.lookup_pc_valid ?
        update.btb_update.lookup_pc : update.btb_update.pc;
}

bpu_addr_t
bpu_v2::commit_cfi_pc(const bpu_commit_update_t& update) const
{
    return commit_lookup_pc(update) +
        static_cast<bpu_addr_t>(
            std::max(0, update.btb_update.branch_sign.offset)) * 2;
}

const btb_entry_record_t*
bpu_v2::find_commit_record(const bpu_commit_update_t& update) const
{
    const int bank_offset =
        std::max(0, update.btb_update.branch_sign.offset) / bank_size_2b;
    const int bank_local_offset =
        std::max(0, update.btb_update.branch_sign.offset) -
        bank_offset * bank_size_2b;

    const auto matches = [&update, bank_local_offset](
        const btb_entry_record_t& record) {
        return record.valid &&
            record.cfi_sign.offset == bank_local_offset &&
            record.cfi_sign.type == update.btb_update.branch_sign.type &&
            record.cfi_sign.compressed ==
                update.btb_update.branch_sign.compressed &&
            record.cfi_sign.is_call ==
                update.btb_update.branch_sign.is_call;
    };

    if (matches(update.btb_update.entry.e1)) {
        return &update.btb_update.entry.e1;
    }
    if (matches(update.btb_update.entry.e2)) {
        return &update.btb_update.entry.e2;
    }

    return nullptr;
}

btb_response_t
bpu_v2::make_response_from_commit(const bpu_commit_update_t& update) const
{
    btb_response_t response;
    if (!update.btb_update.valid) {
        return response;
    }

    const btb_entry_record_t* record = find_commit_record(update);
    if (record == nullptr) {
        return response;
    }

    response.valid = true;
    response.sign = update.btb_update.branch_sign;
    response.target = record->target;
    response.next_cfi_addr = record->taken_next_cfi_addr;
    response.next_cfi_span_2b = record->taken_next_cfi_span_2b;
    response.taken =
        update.btb_update.branch_sign.type == CFI_JAL ||
        update.btb_update.branch_taken;
    if (update.tt_update.valid) {
        response.target = update.tt_update.target;
        response.next_cfi_addr = update.tt_update.target;
        response.taken = true;
        response.tt_hit = true;
    }
    return response;
}

void
bpu_v2::update_abtb_from_commit(const bpu_commit_update_t& update,
                                const btb_commit_result_t& btb_result)
{
    if (!update.btb_update.valid) {
        return;
    }

    const bpu_addr_t lookup_pc = commit_lookup_pc(update);
    const bpu_addr_t cfi_pc = commit_cfi_pc(update);
    const cfi_type_t type = update.btb_update.branch_sign.type;

    if (type == CFI_BRA && update.btb_update.update_branch_ctr &&
        !btb_result.branch_strongly_taken) {
        bpu_abtb.invalidate(cfi_pc);
        return;
    }

    const bool abtb_fillable =
        type == CFI_JAL ||
        (type == CFI_BRA && btb_result.branch_strongly_taken);
    if (!abtb_fillable) {
        return;
    }

    btb_response_t remembered = take_remembered_btb_response(
        update.speculative_id, lookup_pc, cfi_pc);
    if (!remembered.valid) {
        remembered = make_response_from_commit(update);
    }

    if (remembered.valid && remembered.taken) {
        bpu_abtb.insert_or_update(cfi_pc, remembered);
    }
}

void
bpu_v2::retire_speculative_through(int speculative_id)
{
    if (speculative_id < 0) {
        return;
    }

    speculative_nodes.erase(
        std::remove_if(speculative_nodes.begin(), speculative_nodes.end(),
            [speculative_id](const bpu_speculative_node_t& node) {
                return node.id <= speculative_id;
            }),
        speculative_nodes.end());
}

void
bpu_v2::squash_speculative_after(int speculative_id)
{
    squash_speculative_nodes(speculative_id, false);
}

void
bpu_v2::squash_speculative_from(int speculative_id)
{
    squash_speculative_nodes(speculative_id, true);
}

const std::vector<bpu_speculative_node_t>&
bpu_v2::get_speculative_nodes() const
{
    return speculative_nodes;
}

std::size_t
bpu_v2::speculative_node_count() const
{
    return speculative_nodes.size();
}

std::size_t
bpu_v2::ras_depth() const
{
    return ras_stack.size();
}

std::size_t
bpu_v2::tage_checkpoint_count() const
{
    return bpu_tage_predictor.checkpoint_count();
}

std::size_t
bpu_v2::ittage_checkpoint_count() const
{
    return bpu_ittage_predictor.checkpoint_count();
}

bpu_addr_t
bpu_v2::compute_cfi_addr() const
{
    return base_addr + pending_next_cfi_span_2b * 2;
}

int
bpu_v2::bank_align_span_2b(int span_2b) const
{
    if (span_2b <= 0) {
        return 0;
    }

    return (span_2b / bank_size_2b) * bank_size_2b;
}

int
bpu_v2::span_from_next_cfi_addr(bpu_addr_t span_base,
                                bpu_addr_t next_cfi_addr,
                                int fallback_span_2b) const
{
    if (next_cfi_addr != 0 && next_cfi_addr >= span_base &&
        ((next_cfi_addr - span_base) % 2) == 0) {
        return static_cast<int>((next_cfi_addr - span_base) / 2);
    }

    return bank_align_span_2b(fallback_span_2b);
}

bpu_addr_t
bpu_v2::next_cfi_addr_from_span(bpu_addr_t span_base, int span_2b) const
{
    return span_base +
        static_cast<bpu_addr_t>(bank_align_span_2b(span_2b)) * 2;
}

int
bpu_v2::cfi_inst_size_2b(const bpu_sign_t& sign) const
{
    return sign.compressed ? 1 : 2;
}

int
bpu_v2::cfi_offset_from_base_2b(int pending_span_2b,
                                const bpu_sign_t& sign) const
{
    return pending_span_2b + std::max(0, sign.offset);
}

int
bpu_v2::fetch_span_through_taken_2b(int pending_span_2b,
                                    const bpu_sign_t& sign) const
{
    return cfi_offset_from_base_2b(pending_span_2b, sign) +
        cfi_inst_size_2b(sign);
}

bpu_sign_t
bpu_v2::make_ftq_base_sign(int pending_span_2b,
                           const bpu_sign_t& sign) const
{
    bpu_sign_t ftq_sign = sign;
    if (ftq_sign.type == CFI_NULL) {
        ftq_sign.offset = 0;
        return ftq_sign;
    }

    ftq_sign.offset = cfi_offset_from_base_2b(pending_span_2b, sign);
    return ftq_sign;
}

void
bpu_v2::advance_from_prediction(bool taken, bpu_addr_t target,
                                int next_cfi_span_2b,
                                bpu_addr_t next_cfi_addr)
{
    if (taken) {
        base_addr = target;
        pending_next_cfi_span_2b =
            span_from_next_cfi_addr(base_addr, next_cfi_addr,
                                    next_cfi_span_2b);
    }

    cfi_addr = compute_cfi_addr();
}

void
bpu_v2::advance_fallthrough(int next_cfi_span_2b,
                            bpu_addr_t next_cfi_addr)
{
    if (next_cfi_addr != 0 && next_cfi_addr >= base_addr &&
        ((next_cfi_addr - base_addr) % 2) == 0) {
        pending_next_cfi_span_2b =
            static_cast<int>((next_cfi_addr - base_addr) / 2);
    } else {
        const int aligned_span = bank_align_span_2b(next_cfi_span_2b);
        pending_next_cfi_span_2b +=
            aligned_span > 0 ? aligned_span : fetch_block_size_2b;
    }
    cfi_addr = compute_cfi_addr();
}

int
bpu_v2::create_speculative_node(
    bpu_addr_t pc, const btb_response_t& response,
    const std::vector<int>& tage_checkpoint_ids,
    int ittage_checkpoint_id)
{
    bpu_speculative_node_t node;
    node.valid = true;
    node.id = next_speculative_id++;
    node.cfi_addr = pc;
    node.bpu2_response = response;
    node.bpu2_response.speculative_id = node.id;
    node.tage_checkpoint_ids = tage_checkpoint_ids;
    node.ittage_checkpoint_id = ittage_checkpoint_id;
    node.ras_snapshot = ras_stack;
    speculative_nodes.push_back(node);
    trim_speculative_nodes();
    return node.id;
}

bpu_speculative_node_t*
bpu_v2::find_speculative_node(int speculative_id)
{
    for (bpu_speculative_node_t& node : speculative_nodes) {
        if (node.valid && node.id == speculative_id) {
            return &node;
        }
    }

    return nullptr;
}

void
bpu_v2::mark_bpu3_resolved(int speculative_id)
{
    bpu_speculative_node_t* node = find_speculative_node(speculative_id);
    if (node == nullptr) {
        return;
    }

    node->resolved_by_bpu3 = true;
}

void
bpu_v2::mark_ittage_checkpoint(int speculative_id, int checkpoint_id)
{
    bpu_speculative_node_t* node = find_speculative_node(speculative_id);
    if (node == nullptr) {
        return;
    }

    node->ittage_checkpoint_id = checkpoint_id;
}

ras_response_t
bpu_v2::make_ras_response(ras_response_t response) const
{
    if (ras_enabled && !response.valid && !ras_stack.empty()) {
        response.valid = true;
        response.target = ras_stack.back();
    }

    return response;
}

void
bpu_v2::update_ras_from_prediction(bpu_addr_t pc,
                                   const btb_response_t& response)
{
    if (!response.valid || !ras_enabled) {
        return;
    }

    if ((response.sign.type == CFI_JAL ||
         response.sign.type == CFI_JALR_CALL) &&
        response.sign.is_call) {
        const bpu_addr_t cfi_pc =
            pc + std::max(0, response.sign.offset) * 2;
        const bpu_addr_t return_pc =
            cfi_pc + cfi_inst_size_2b(response.sign) * 2;
        push_ras(return_pc);
        return;
    }

    if (response.sign.type == CFI_JALR_RET && !ras_stack.empty()) {
        ras_stack.pop_back();
    }
}

void
bpu_v2::trim_speculative_nodes()
{
    while (speculative_nodes.size() > max_speculative_nodes) {
        speculative_nodes.erase(speculative_nodes.begin());
    }
}

void
bpu_v2::push_ras(bpu_addr_t return_pc)
{
    if (ras_stack.size() >= max_ras_depth) {
        ras_stack.erase(ras_stack.begin());
    }
    ras_stack.push_back(return_pc);
}

void
bpu_v2::squash_speculative_nodes(int speculative_id, bool include_self)
{
    if (speculative_id < 0) {
        return;
    }

    for (const bpu_speculative_node_t& node : speculative_nodes) {
        const bool should_squash = include_self ?
            node.id >= speculative_id : node.id > speculative_id;
        if (should_squash) {
            ras_stack = node.ras_snapshot;
            break;
        }
    }

    speculative_nodes.erase(
        std::remove_if(speculative_nodes.begin(), speculative_nodes.end(),
            [this, speculative_id, include_self](
                const bpu_speculative_node_t& node) {
                const bool should_squash = include_self ?
                    node.id >= speculative_id : node.id > speculative_id;
                if (!should_squash) {
                    return false;
                }

                if (!node.tage_checkpoint_ids.empty()) {
                    bpu_tage_predictor.squash_checkpoint(
                        node.tage_checkpoint_ids.front(), true);
                }
                if (node.ittage_checkpoint_id >= 0) {
                    bpu_ittage_predictor.squash_checkpoint(
                        node.ittage_checkpoint_id, true);
                }
                return true;
            }),
        speculative_nodes.end());
}
