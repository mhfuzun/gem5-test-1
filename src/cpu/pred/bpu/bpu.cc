#include <algorithm>

#include "bpu.hh"

bpu::bpu(btb_cfg btb_cfg, ubtb_cfg ubtb_cfg, tt_cfg tt_cfg,
         ftq_cfg ftq_cfg, bool enable_tage, bool enable_ras,
         bool enable_ittage)
    : bpu_ubtb(ubtb_cfg),
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
bpu::reset()
{
    base_addr = 0;
    cfi_addr = 0;
    pending_next_cfi_span_2b = 0;
    bpu1_opens_new_ftq_entry = true;
    bpu_ftq.clear();
    speculative_nodes.clear();
    ras_stack.clear();
    bpu_tage_predictor.reset();
    bpu_ittage_predictor.reset();
}

void
bpu::set_base_addr(bpu_addr_t pc)
{
    base_addr = pc;
    pending_next_cfi_span_2b = 0;
    bpu1_opens_new_ftq_entry = true;
    cfi_addr = compute_cfi_addr();
}

bpu_cycle_output_t
bpu::tick(const bpu_cycle_input_t& input)
{
    if (input.base_valid) {
        set_base_addr(input.base_addr);
    }

    cfi_addr = compute_cfi_addr();
    const bpu_addr_t lookup_cfi_addr = cfi_addr;
    bpu_cycle_output_t output;
    output.lookup_cfi_addr = lookup_cfi_addr;

    tt_bank_response_t tt_response = input.tt_response;
    if (tt_response.banks.empty()) {
        tt_response = bpu_tt.lookup(lookup_cfi_addr);
    }

    const int ftq_size_before_bpu1 = bpu_ftq.size();
    const int bpu1_old_size = bpu_ftq.size();
    const ftq_entry_t* bpu1_old_entry = bpu_ftq.back();
    output.bpu1_old_fetch_span_2b =
        bpu1_old_entry == nullptr ? 0 : bpu1_old_entry->fetch_span_2b;
    output.bpu1 = run_bpu1(lookup_cfi_addr);
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
        output.bpu1.target +
            bank_align_span_2b(output.bpu1.next_cfi_span_2b) * 2 :
        base_addr + output.bpu1_new_fetch_span_2b * 2;

    tage_response_t tage_response = input.tage_response;
    if (input.use_tage && tage_enabled && !tage_response.valid) {
        tage_response =
            bpu_tage_predictor.predict(
                bpu_btb.lookup_tage_slots(lookup_cfi_addr));
    }
    output.tage_response = tage_response;

    const ftq_entry_t* bpu2_old_entry = bpu_ftq.back();
    output.bpu2_old_fetch_span_2b =
        bpu2_old_entry == nullptr ? 0 : bpu2_old_entry->fetch_span_2b;
    output.bpu2 = run_bpu2(lookup_cfi_addr, tage_response,
                           tt_response, input.ras_response);
    const ftq_entry_t* bpu2_new_entry = bpu_ftq.back();
    output.bpu2_new_fetch_span_2b =
        bpu2_new_entry == nullptr ? 0 : bpu2_new_entry->fetch_span_2b;
    output.bpu2_next_cfi_addr =
        output.bpu2.valid && output.bpu2.taken ?
        output.bpu2.target +
            bank_align_span_2b(output.bpu2.next_cfi_span_2b) * 2 :
        output.bpu2.valid ?
            base_addr + output.bpu2_new_fetch_span_2b * 2 : 0;
    if (!output.bpu1.valid && output.bpu2.valid &&
        output.bpu2.taken && output.bpu2.ubtb_fillable) {
        const ubtb_entry_t fill_entry =
            bpu_btb.make_ubtb_entry(lookup_cfi_addr, output.bpu2);
        if (fill_entry.valid) {
            bpu_ubtb.insert_or_update(lookup_cfi_addr, fill_entry);
            mark_ubtb_fill(output.bpu2.speculative_id, lookup_cfi_addr);
            output.ubtb_filled = true;
        }
    }

    ittage_response_t ittage_response = input.ittage_response;
    if (input.use_ittage && ittage_enabled &&
        ittage_response.checkpoint_id < 0 && output.bpu2.valid &&
        output.bpu2.sign.type == CFI_JALR_CALL) {
        const bpu_addr_t jalr_pc =
            lookup_cfi_addr + output.bpu2.sign.offset * 2;
        ittage_response = bpu_ittage_predictor.lookup(jalr_pc);
        mark_ittage_checkpoint(output.bpu2.speculative_id,
                               ittage_response.checkpoint_id);
    }
    output.ittage_response = ittage_response;

    const ftq_entry_t* bpu3_old_entry = bpu_ftq.back();
    output.bpu3_old_fetch_span_2b =
        bpu3_old_entry == nullptr ? 0 : bpu3_old_entry->fetch_span_2b;
    output.redirect = run_bpu3(lookup_cfi_addr, output.bpu2,
                               ittage_response);
    output.bpu3_redirect = output.redirect.valid;
    const ftq_entry_t* bpu3_new_entry = bpu_ftq.back();
    output.bpu3_new_fetch_span_2b =
        bpu3_new_entry == nullptr ? 0 : bpu3_new_entry->fetch_span_2b;
    output.bpu3_next_cfi_addr = output.redirect.valid ?
        output.redirect.target +
            bank_align_span_2b(output.redirect.next_cfi_span_2b) * 2 : 0;

    output.ftq_pushed = bpu_ftq.size() > ftq_size_before_bpu1;
    output.ftq_updated = output.bpu2.valid || output.redirect.valid;

    if (output.redirect.valid) {
        advance_from_prediction(true, output.redirect.target,
                                output.redirect.next_cfi_span_2b);
    } else if (output.bpu2.taken) {
        output.redirect.valid = true;
        output.redirect.target = output.bpu2.target;
        output.redirect.next_cfi_span_2b = output.bpu2.next_cfi_span_2b;
        output.redirect.sign = output.bpu2.sign;
        advance_from_prediction(true, output.bpu2.target,
                                output.bpu2.next_cfi_span_2b);
    } else if (output.bpu2.valid &&
               ((output.bpu2.sign.type == CFI_JALR_CALL &&
                 !output.bpu2.tt_hit) ||
                (output.bpu2.sign.type == CFI_JALR_RET &&
                 !output.bpu2.ras_valid))) {
        cfi_addr = compute_cfi_addr();
    } else if (output.bpu2.valid) {
        advance_fallthrough(output.bpu2.next_cfi_span_2b);
    } else if (output.bpu1.taken) {
        advance_from_prediction(true, output.bpu1.target,
                                output.bpu1.next_cfi_span_2b);
    } else {
        advance_fallthrough(output.bpu1.next_cfi_span_2b);
    }

    bpu1_opens_new_ftq_entry = output.redirect.valid || output.bpu1.taken;
    return output;
}

ubtb_response_t
bpu::run_bpu1(bpu_addr_t pc)
{
    ubtb_response_t ubtb_response = bpu_ubtb.predict(pc);
    // ubtb oku, sonucu ftq içine base adres ve span olarak yaz.
    // Bu stage hızlı yol olduğu için yalnızca uBTB'nin verdiği erken block
    // bilgisini FTQ'ye taşır. Daha güvenilir BTB/TAGE/TT/RAS sonucu BPU2'de
    // aynı FTQ entry'sini düzeltir; burada amaç fetch tarafını boş bırakmadan
    // ilerideki muhtemel CFI bloğuna doğru kaba bir yol haritası üretmek.
    // fetch_span_2b, IFU'nun base_addr'den itibaren kaç 16-bit parça çekeceği;
    // next_cfi_span_2b ise redirect target/base üzerinden bir sonraki CFI
    // fetch block'una bank-aligned yürüyüş mesafesidir.
    const int fetch_span_2b = ubtb_response.valid && ubtb_response.taken ?
        fetch_span_through_taken_2b(ubtb_response.sign) :
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
        entry->target = ubtb_response.valid && ubtb_response.taken ?
            ubtb_response.target : 0;
        entry->next_cfi_span_2b =
            bank_align_span_2b(ubtb_response.next_cfi_span_2b);
        entry->cfi_taken_sign = ubtb_response.valid && ubtb_response.taken ?
            make_ftq_base_sign(ubtb_response.sign) : bpu_sign_t{};
    }

    return ubtb_response;
}

btb_response_t
bpu::run_bpu2(bpu_addr_t pc, tage_response_t tage_response,
              tt_bank_response_t tt_response, ras_response_t ras_response)
{
    // ...

    /**
     * btb, TAGE, RAS (ToS), TT'den yanıt al,
     * yanıtı btb yanıtı ile karşılaştır, bir farklılık varsa düzelt.
     * span değerini doğru olacak şekilde ayarla (ftq içindeki)
     * bpu redirect yap, eğer ftq isteği okunup işlem yapılmış ise
     * şuan ilk okunduktan sonraki noktadır (pop->use it şeklinde), onu da
     * denetle
     */
    /*
     * BPU2 burada bank hizalı sonuçları birleştirir. BTB lookup, fetch bloğu
     * içindeki bank sırasını koruyarak CFI kayıtlarını döndürür; TT de aynı
     * istek PC'si için bank sayısı kadar hit/target cevabı üretir. Bir JALR
     * call kaydı seçilirse BTB kaydının bulunduğu bank slotu ile aynı TT slotu
     * okunur. Böylece TT tek global target gibi davranmaz; BTB'nin banklanmış
     * CFI görüşüyle aynı fiziksel hizayı takip eder.
     *
     * Bu fonksiyon şu an yalnızca FTQ front entry'sini düzeltir. Gem5
     * entegrasyonunda IFU bu entry'yi tüketmişse redirect çıktısı üzerinden
     * fetch PC düzeltmesi yapılmalı; SHQ/CFI tracer geldiğinde aynı noktada
     * predictor metadata id'leri de bağlanacak.
     *
     * Taken bir kayıt seçildiğinde FTQ fetch_span_2b, önceki redirect'in
     * next_cfi_span_2b değeri + bu block içindeki CFI offset'i + instruction
     * uzunluğu olarak hesaplanır. Böylece IFU, taken CFI'ın kendisini de
     * fetch eder; bir sonraki base target olur ve yeni CFI probe adresi
     * target + next_cfi_span_2b*2 şeklinde bulunur.
     */

    ras_response_t effective_ras_response = make_ras_response(ras_response);
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
            create_bpu2_speculative_node(pc, btb_response,
                                         tage_checkpoint_ids, -1);
        update_ras_from_prediction(pc, btb_response);
    } else if (tage_response.valid) {
        bpu_tage_predictor.discard_response(tage_response);
    }

    ftq_entry_t* entry = bpu_ftq.back();
    if (entry != nullptr && btb_response.valid) {
        const bpu_sign_t ftq_base_sign =
            make_ftq_base_sign(btb_response.sign);
        const bool is_indirect =
            btb_response.sign.type == CFI_JALR_CALL ||
            btb_response.sign.type == CFI_JALR_RET;
        const bool stops_at_response = btb_response.taken || is_indirect;
        const bool unresolved_indirect = is_indirect && !btb_response.taken;

        if (stops_at_response) {
            entry->fetch_span_2b = std::max(
                fetch_span_through_taken_2b(btb_response.sign),
                entry->consumed_span_2b);
        } else if (btb_response.sign.type == CFI_BRA) {
            entry->fetch_span_2b = std::max(
                pending_next_cfi_span_2b +
                    bank_align_span_2b(btb_response.next_cfi_span_2b),
                entry->consumed_span_2b);
        }

        entry->target = btb_response.target;
        entry->next_cfi_span_2b =
            bank_align_span_2b(btb_response.next_cfi_span_2b);
        entry->jalr_fail = unresolved_indirect;
        entry->cfi_taken_sign = stops_at_response ? ftq_base_sign :
            bpu_sign_t{};
        entry->speculative_id = btb_response.speculative_id;
    }

    return btb_response;
}

bpu_redirect_t
bpu::run_bpu3(bpu_addr_t pc, const btb_response_t& bpu2_response,
              ittage_response_t ittage_response)
{
    // ...

    // burada sadece ITTAGE, target değişkliği yapar,
    // bpu1, ve bpu2 flush'lanır. yaptıkları yazma işlemleri engellenir.
    // eğer bpu1'deki target yanlış adres ile IFU'de işlenmeye başlamış ise
    // bunu yakala ve düzelt.
    // ITTAGE yanıtı BPU2'den daha geç geldiği için burada yalnızca target
    // doğruluğu ile ilgilenmek gerekir. Yön/CFI seçimi BPU2'de sabit kalır;
    // ileride ITTAGE target değiştirdiğinde genç BPU1/BPU2 yazmaları iptal
    // edilip FTQ ve fetch tarafına geç target redirect'i gönderilecek.
    (void)pc;

    bpu_redirect_t redirect;
    ftq_entry_t* entry = bpu_ftq.back();
    mark_bpu3_resolved(bpu2_response.speculative_id);

    if (entry == nullptr ||
        bpu2_response.sign.type != CFI_JALR_CALL) {
        return redirect;
    }

    if (ittage_response.hit) {
        const bool needs_late_redirect =
            !bpu2_response.tt_hit ||
            bpu2_response.target != ittage_response.target;
        entry->target = ittage_response.target;
        entry->next_cfi_span_2b =
            bank_align_span_2b(ittage_response.next_cfi_span_2b);
        entry->fetch_span_2b = std::max(
            fetch_span_through_taken_2b(bpu2_response.sign),
            entry->consumed_span_2b);
        entry->jalr_fail = false;

        if (!needs_late_redirect) {
            return redirect;
        }

        redirect.valid = true;
        redirect.target = ittage_response.target;
        redirect.next_cfi_span_2b = entry->next_cfi_span_2b;
        redirect.sign = bpu2_response.sign;
        squash_speculative_after(bpu2_response.speculative_id);
        return redirect;
    }

    if (!bpu2_response.tt_hit) {
        bpu_ftq.set_jalr_fail(*entry, true);
    }
    return redirect;
}

void
bpu::consume_ftq(int two_byte_count)
{
    bpu_ftq.consume(two_byte_count);
}

int
bpu::get_fetch_span() const
{
    return bpu_ftq.get_fetch_span_2b();
}

const ftq_entry_t*
bpu::get_ftq_front() const
{
    return bpu_ftq.front();
}

bool
bpu::ftq_ready() const
{
    return bpu_ftq.ready();
}

bool
bpu::ftq_empty() const
{
    return !bpu_ftq.ready();
}

bool
bpu::ftq_full() const
{
    return bpu_ftq.full();
}

void
bpu::recover(bpu_addr_t pc, int speculative_id, bool include_self)
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
bpu::update_ubtb(bpu_addr_t pc, const ubtb_entry_t& entry)
{
    bpu_ubtb.insert_or_update(pc, entry);
}

void
bpu::update_btb(bpu_addr_t pc, const btb_entry_t& entry)
{
    bpu_btb.insert_or_update(pc, entry);
}

void
bpu::update_tt(bpu_addr_t pc, bpu_addr_t target)
{
    bpu_tt.insert_or_update(pc, target);
}

void
bpu::commit(const bpu_commit_update_t& update)
{
    const btb_commit_result_t btb_result =
        bpu_btb.commit(update.btb_update);
    if (update.btb_update.valid && update.btb_update.update_branch_ctr &&
        btb_result.branch_ctr_updated &&
        !btb_result.branch_strongly_taken) {
        const bpu_addr_t ubtb_pc = update.btb_update.ubtb_pc_valid ?
            update.btb_update.ubtb_pc : update.btb_update.pc;
        bpu_ubtb.invalidate(ubtb_pc);
    }

    bpu_tt.commit(update.tt_update);

    const bpu_addr_t lookup_pc = update.btb_update.lookup_pc_valid ?
        update.btb_update.lookup_pc : update.btb_update.pc;
    const bpu_addr_t cfi_pc =
        lookup_pc + std::max(0, update.btb_update.branch_sign.offset) * 2;

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
bpu::squash_speculative_after(int speculative_id)
{
    squash_speculative_nodes(speculative_id, false);
}

void
bpu::squash_speculative_from(int speculative_id)
{
    squash_speculative_nodes(speculative_id, true);
}

const std::vector<bpu_speculative_node_t>&
bpu::get_speculative_nodes() const
{
    return speculative_nodes;
}

std::size_t
bpu::speculative_node_count() const
{
    return speculative_nodes.size();
}

std::size_t
bpu::ras_depth() const
{
    return ras_stack.size();
}

std::size_t
bpu::tage_checkpoint_count() const
{
    return bpu_tage_predictor.checkpoint_count();
}

std::size_t
bpu::ittage_checkpoint_count() const
{
    return bpu_ittage_predictor.checkpoint_count();
}

bpu_addr_t
bpu::compute_cfi_addr() const
{
    return base_addr + pending_next_cfi_span_2b * 2;
}

int
bpu::bank_align_span_2b(int span_2b) const
{
    if (span_2b <= 0) {
        return 0;
    }

    return (span_2b / bank_size_2b) * bank_size_2b;
}

int
bpu::cfi_inst_size_2b(const bpu_sign_t& sign) const
{
    return sign.compressed ? 1 : 2;
}

int
bpu::cfi_offset_from_base_2b(const bpu_sign_t& sign) const
{
    return pending_next_cfi_span_2b + std::max(0, sign.offset);
}

int
bpu::fetch_span_through_taken_2b(const bpu_sign_t& sign) const
{
    return cfi_offset_from_base_2b(sign) + cfi_inst_size_2b(sign);
}

bpu_sign_t
bpu::make_ftq_base_sign(const bpu_sign_t& sign) const
{
    bpu_sign_t ftq_sign = sign;
    if (ftq_sign.type == CFI_NULL) {
        ftq_sign.offset = 0;
        return ftq_sign;
    }

    // BTB/uBTB cevapları lookup edilen CFI fetch block'una göre offset taşır.
    // FTQ entry'si ise ilk base_addr sabit kalacak şekilde büyür; bu yüzden
    // decode tarafında prediction eşleştirmesi yapabilmek için CFI offset'i
    // entry base'ine göre normalize edilmiş olarak saklanır.
    ftq_sign.offset = cfi_offset_from_base_2b(sign);
    return ftq_sign;
}

void
bpu::advance_from_prediction(bool taken, bpu_addr_t target,
                             int next_cfi_span_2b)
{
    if (taken) {
        base_addr = target;
        pending_next_cfi_span_2b = bank_align_span_2b(next_cfi_span_2b);
    }

    cfi_addr = compute_cfi_addr();
}

void
bpu::advance_fallthrough(int next_cfi_span_2b)
{
    const int aligned_span = bank_align_span_2b(next_cfi_span_2b);
    pending_next_cfi_span_2b +=
        aligned_span > 0 ? aligned_span : fetch_block_size_2b;
    cfi_addr = compute_cfi_addr();
}

int
bpu::create_bpu2_speculative_node(
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
bpu::find_speculative_node(int speculative_id)
{
    for (bpu_speculative_node_t& node : speculative_nodes) {
        if (node.valid && node.id == speculative_id) {
            return &node;
        }
    }

    return nullptr;
}

void
bpu::mark_bpu3_resolved(int speculative_id)
{
    bpu_speculative_node_t* node = find_speculative_node(speculative_id);
    if (node == nullptr) {
        return;
    }

    node->resolved_by_bpu3 = true;
}

void
bpu::mark_ittage_checkpoint(int speculative_id, int checkpoint_id)
{
    bpu_speculative_node_t* node = find_speculative_node(speculative_id);
    if (node == nullptr) {
        return;
    }

    node->ittage_checkpoint_id = checkpoint_id;
}

void
bpu::mark_ubtb_fill(int speculative_id, bpu_addr_t pc)
{
    bpu_speculative_node_t* node = find_speculative_node(speculative_id);
    if (node == nullptr) {
        return;
    }

    node->ubtb_fill_valid = true;
    node->ubtb_fill_pc = pc;
}

ras_response_t
bpu::make_ras_response(ras_response_t response) const
{
    if (ras_enabled && !response.valid && !ras_stack.empty()) {
        response.valid = true;
        response.target = ras_stack.back();
    }

    return response;
}

void
bpu::update_ras_from_prediction(bpu_addr_t pc,
                                const btb_response_t& response)
{
    if (!response.valid) {
        return;
    }

    if (!ras_enabled) {
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
bpu::retire_speculative_through(int speculative_id)
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
bpu::trim_speculative_nodes()
{
    while (speculative_nodes.size() > max_speculative_nodes) {
        speculative_nodes.erase(speculative_nodes.begin());
    }
}

void
bpu::push_ras(bpu_addr_t return_pc)
{
    if (ras_stack.size() >= max_ras_depth) {
        ras_stack.erase(ras_stack.begin());
    }
    ras_stack.push_back(return_pc);
}

void
bpu::squash_speculative_nodes(int speculative_id, bool include_self)
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

                if (node.ubtb_fill_valid) {
                    bpu_ubtb.invalidate(node.ubtb_fill_pc);
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
