#include <algorithm>
#include <cstddef>

#include "btb.hh"

namespace
{

int
positive_count(int value)
{
    return std::max(1, value);
}

bpu_addr_t
mask_bits(int width)
{
    if (width <= 0) {
        return 0;
    }

    if (width >= static_cast<int>(sizeof(bpu_addr_t) * 8)) {
        return ~bpu_addr_t{0};
    }

    return (bpu_addr_t{1} << width) - 1;
}

} // namespace

btb::btb(btb_cfg cfg)
{
    this->cfg = cfg;

    const int ways = positive_count(cfg.way_count);
    const int banks = positive_count(cfg.bank_count);
    const int sets = positive_count(cfg.set_count);

    replacement.reset(ways);
    replacement_states.resize(banks);
    for (auto& bank : replacement_states) {
        bank.resize(sets);
        for (auto& state : bank) {
            state = replacement.new_state();
        }
    }

    btb_table.resize(ways);
    for (auto& way : btb_table) {
        way.resize(banks);
        for (auto& bank : way) {
            bank.resize(sets);
        }
    }
}

int
btb::get_bank_index(bpu_addr_t pc) const
{
    const int banks = positive_count(cfg.bank_count);
    const bpu_addr_t bank_block =
        (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    return static_cast<int>(bank_block % static_cast<bpu_addr_t>(banks));
}

int
btb::generate_index(bpu_addr_t pc) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);
    bpu_addr_t block = (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return static_cast<int>(block % static_cast<bpu_addr_t>(sets));
}

bpu_addr_t
btb::generate_tag(bpu_addr_t pc) const
{
    const int banks = positive_count(cfg.bank_count);
    bpu_addr_t block = (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return block & mask_bits(cfg.tag_width);
}

int
btb::get_bank_2b_count() const
{
    const int banks = positive_count(cfg.bank_count);
    return std::max(1, bpu_cfg::fetch_block_2b_count / banks);
}

bpu_sign_t
btb::make_fetch_block_sign(const bpu_sign_t& bank_sign,
                           int fetch_block_offset_2b) const
{
    bpu_sign_t sign = bank_sign;
    sign.offset = fetch_block_offset_2b;
    return sign;
}

bool
btb::is_strongly_taken(const btb_entry_record_t& entry) const
{
    return entry.branch_ctr >= bpu_cfg::btb_branch_ctr_strong_taken;
}

bool
btb::is_direct_fillable(const btb_entry_record_t& entry) const
{
    if (entry.cfi_sign.type == CFI_JAL) {
        return true;
    }

    if (entry.cfi_sign.type == CFI_BRA) {
        return is_strongly_taken(entry);
    }

    return false;
}

std::vector<btb::btb_lookup_bank_t>
btb::lookup(bpu_addr_t pc) const
{
    std::vector<btb_lookup_bank_t> result;
    result.reserve(positive_count(cfg.bank_count));

    const int pc_bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const int next_idx = (idx + 1) % positive_count(cfg.set_count);
    const bpu_addr_t tag = generate_tag(pc);
    const int bank_2b_count = get_bank_2b_count();
    const int lookup_base_offset_2b =
        ((pc >> cfg.tag_pc_shift) % bank_2b_count + bank_2b_count) %
        bank_2b_count;

    for (int bank_offset = 0; bank_offset < positive_count(cfg.bank_count);
         ++bank_offset) {
        const int bank =
            (pc_bank + bank_offset) % positive_count(cfg.bank_count);
        const int set = bank < pc_bank ? next_idx : idx;
        btb_lookup_bank_t bank_result;
        bank_result.bank = bank;
        bank_result.bank_slot = bank_offset;

        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            const btb_entry_t& entry = btb_table[way][bank][set];
            // The tag covers the banked BTB group, but the experimental FTQ
            // can start a logical fetch block at any halfword inside that
            // group. Keep the original logical base offset in the match so
            // entries learned for neighbouring unaligned bases do not alias
            // and report a CFI offset a few halfwords early.
            if (!entry.valid || entry.tag != tag ||
                entry.base_offset_2b != lookup_base_offset_2b) {
                continue;
            }

            const auto push_record =
                [&bank_result, bank_offset, bank_2b_count, way, set](
                    const btb_entry_record_t& record,
                    std::size_t record_slot) {
                    // cfi_tracer stores record.offset bank-local relative to
                    // the logical fetch block, not relative to the physical
                    // bank containing the lookup PC. Therefore the response
                    // offset is logical_bank_start + record-local offset.
                    const int fetch_block_offset_2b =
                        bank_offset * bank_2b_count +
                        record.cfi_sign.offset;
                    if (fetch_block_offset_2b < 0 ||
                        fetch_block_offset_2b >=
                        bpu_cfg::fetch_block_2b_count) {
                        return;
                    }

                    btb_lookup_record_t lookup_record;
                    lookup_record.record = &record;
                    lookup_record.fetch_block_offset_2b =
                        fetch_block_offset_2b;
                    lookup_record.way = way;
                    lookup_record.set = set;
                    lookup_record.tt_bank_slot = bank_offset;
                    lookup_record.tage_slot = bank_offset * 2 + record_slot;
                    bank_result.records.push_back(lookup_record);
                };

            if (entry.e1.valid) {
                push_record(entry.e1, 0);
            }

            if (entry.e2.valid) {
                push_record(entry.e2, 1);
            }
        }

        result.push_back(bank_result);
    }

    return result;
}

std::vector<tage_lookup_slot_t>
btb::lookup_tage_slots(bpu_addr_t pc) const
{
    const int banks = positive_count(cfg.bank_count);
    std::vector<tage_lookup_slot_t> slots(static_cast<std::size_t>(banks) * 2);
    const std::vector<btb_lookup_bank_t> bank_entries = lookup(pc);

    for (const btb_lookup_bank_t& bank_entry : bank_entries) {
        for (const btb_lookup_record_t& lookup_record : bank_entry.records) {
            const btb_entry_record_t* entry = lookup_record.record;
            if (entry == nullptr || !entry->valid ||
                entry->cfi_sign.type != CFI_BRA ||
                lookup_record.tage_slot >= slots.size()) {
                continue;
            }

            tage_lookup_slot_t& slot = slots[lookup_record.tage_slot];
            if (slot.valid) {
                continue;
            }

            slot.valid = true;
            slot.sign = make_fetch_block_sign(
                entry->cfi_sign, lookup_record.fetch_block_offset_2b);
            slot.pc = pc + slot.sign.offset * 2;
        }
    }

    return slots;
}

void
btb::predict(btb_response_t& response, bpu_addr_t pc,
             tage_response_t tage_response, tt_bank_response_t tt_response,
             ras_response_t ras_response)
{
    response = predict(pc, tage_response, tt_response, ras_response);
}

btb_response_t
btb::predict(bpu_addr_t pc, tage_response_t tage_response,
             tt_bank_response_t tt_response,
             ras_response_t ras_response)
{
    const std::vector<btb_lookup_bank_t> bank_entries = lookup(pc);
    btb_response_t fallthrough_response;

    for (std::size_t bank_offset = 0; bank_offset < bank_entries.size();
         ++bank_offset) {
        const btb_lookup_bank_t& bank_entry = bank_entries[bank_offset];

        for (const btb_lookup_record_t& lookup_record : bank_entry.records) {
            const btb_entry_record_t* entry = lookup_record.record;
            if (entry == nullptr || !entry->valid) {
                continue;
            }

            if (bank_entry.bank >= 0 &&
                static_cast<std::size_t>(bank_entry.bank) <
                    replacement_states.size() &&
                lookup_record.set <
                    replacement_states[bank_entry.bank].size()) {
                replacement.touch(
                    replacement_states[bank_entry.bank][lookup_record.set],
                    lookup_record.way);
            }

            const bpu_sign_t fetch_block_sign = make_fetch_block_sign(
                entry->cfi_sign, lookup_record.fetch_block_offset_2b);

            if (entry->cfi_sign.type == CFI_BRA) {
                const bool tage_slot_valid =
                    tage_response.valid &&
                    lookup_record.tage_slot < tage_response.predictions.size()
                    &&
                    lookup_record.tage_slot < tage_response.slot_valid.size()
                    &&
                    tage_response.slot_valid[lookup_record.tage_slot];
                const bool tage_taken = tage_slot_valid &&
                    tage_response.predictions[lookup_record.tage_slot];
                const int tage_checkpoint_id =
                    tage_slot_valid &&
                    lookup_record.tage_slot <
                    tage_response.checkpoint_ids.size() ?
                    tage_response.checkpoint_ids[lookup_record.tage_slot] :
                    -1;
                const bool ctr_taken = entry->branch_ctr >= 2;
                const bool use_tage_direction =
                    tage_slot_valid && tage_taken == ctr_taken;
                const bool taken =
                    use_tage_direction ? tage_taken : ctr_taken;

                if (taken) {
                    btb_response_t response;
                    response.valid = true;
                    response.taken = true;
                    response.target = entry->target;
                    response.next_cfi_span_2b =
                        entry->taken_next_cfi_span_2b;
                    response.sign = fetch_block_sign;
                    response.tage_used = use_tage_direction;
                    response.tage_slot =
                        static_cast<int>(lookup_record.tage_slot);
                    response.tage_checkpoint_id = tage_checkpoint_id;
                    response.branch_strongly_taken =
                        is_strongly_taken(*entry);
                    response.ubtb_fillable = is_direct_fillable(*entry);
                    return response;
                }

                fallthrough_response.valid = true;
                fallthrough_response.taken = false;
                fallthrough_response.next_cfi_span_2b =
                    entry->fallthrough_next_cfi_span_2b;
                fallthrough_response.sign = fetch_block_sign;
                fallthrough_response.tage_used = use_tage_direction;
                fallthrough_response.tage_slot =
                    static_cast<int>(lookup_record.tage_slot);
                fallthrough_response.tage_checkpoint_id = tage_checkpoint_id;
                fallthrough_response.branch_strongly_taken =
                    is_strongly_taken(*entry);
            } else if (entry->cfi_sign.type == CFI_JAL) {
                btb_response_t response;
                response.valid = true;
                response.taken = true;
                response.target = entry->target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = fetch_block_sign;
                response.ubtb_fillable = is_direct_fillable(*entry);
                return response;
            } else if (entry->cfi_sign.type == CFI_JALR_CALL) {
                const tt_response_t bank_tt =
                    lookup_record.tt_bank_slot < tt_response.banks.size() ?
                    tt_response.banks[lookup_record.tt_bank_slot] :
                    tt_response_t{};

                btb_response_t response;
                response.valid = true;
                response.taken = bank_tt.hit;
                response.target = bank_tt.target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = fetch_block_sign;
                response.tt_hit = bank_tt.hit;
                return response;
            } else if (entry->cfi_sign.type == CFI_JALR_RET) {
                btb_response_t response;
                response.valid = true;
                response.taken = ras_response.valid;
                response.target = ras_response.target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = fetch_block_sign;
                response.ras_valid = ras_response.valid;
                return response;
            }
        }
    }

    if (fallthrough_response.valid) {
        return fallthrough_response;
    }

    return {};
}

void
btb::insert_or_update(bpu_addr_t pc, const btb_entry_t& new_entry)
{
    insert_or_update(pc, new_entry, pc);
}

void
btb::insert_or_update(bpu_addr_t pc, const btb_entry_t& new_entry,
                      bpu_addr_t tag_pc)
{
    if (btb_table.empty()) {
        return;
    }

    const int bank = get_bank_index(pc);
    const int idx = generate_index(pc);

    const bpu_addr_t tag = generate_tag(tag_pc);
    const int base_offset_2b =
        ((tag_pc >> cfg.tag_pc_shift) % get_bank_2b_count() +
         get_bank_2b_count()) % get_bank_2b_count();

    btb_entry_t* entry = nullptr;
    std::size_t entry_way = 0;
    for (int way = 0; way < positive_count(cfg.way_count); ++way) {
        btb_entry_t& candidate = btb_table[way][bank][idx];
        if (candidate.valid && candidate.tag == tag &&
            candidate.base_offset_2b == base_offset_2b) {
            entry = &candidate;
            entry_way = static_cast<std::size_t>(way);
            break;
        }
    }

    if (entry == nullptr) {
        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            btb_entry_t& candidate = btb_table[way][bank][idx];
            if (!candidate.valid) {
                entry = &candidate;
                entry_way = static_cast<std::size_t>(way);
                break;
            }
        }
    }

    std::vector<std::uint8_t>& plru_state = replacement_states[bank][idx];
    if (entry == nullptr) {
        entry_way = replacement.get_lru_and_touch(plru_state);
        entry = &btb_table[entry_way][bank][idx];
    }

    if (!entry->valid || entry->tag != tag ||
        entry->base_offset_2b != base_offset_2b) {
        *entry = new_entry;
        entry->valid = true;
        entry->tag = tag;
        entry->base_offset_2b = base_offset_2b;
        replacement.touch(plru_state, entry_way);
        return;
    }

    replacement.touch(plru_state, entry_way);

    const auto same_record =
        [](const btb_entry_record_t& a, const btb_entry_record_t& b) {
            return a.valid && b.valid &&
                a.cfi_sign.offset == b.cfi_sign.offset &&
                a.cfi_sign.type == b.cfi_sign.type &&
                a.cfi_sign.compressed == b.cfi_sign.compressed &&
                a.cfi_sign.is_call == b.cfi_sign.is_call;
        };

    const auto merge_record =
        [&same_record](btb_entry_record_t& dst,
                       const btb_entry_record_t& src) {
            if (!src.valid) {
                return true;
            }

            if (!dst.valid) {
                dst = src;
                return true;
            }

            if (!same_record(dst, src)) {
                return false;
            }

            const int old_branch_ctr = dst.branch_ctr;
            dst = src;
            dst.branch_ctr = old_branch_ctr;
            return true;
        };

    if (!merge_record(entry->e1, new_entry.e1) &&
        !merge_record(entry->e2, new_entry.e1)) {
        entry->e2 = new_entry.e1;
    }

    if (!merge_record(entry->e1, new_entry.e2) &&
        !merge_record(entry->e2, new_entry.e2)) {
        entry->e2 = new_entry.e2;
    }

    entry->valid = true;
    entry->tag = tag;
    entry->base_offset_2b = base_offset_2b;
}

btb_entry_record_t*
btb::find_branch_record(bpu_addr_t pc, const bpu_sign_t& branch_sign)
{
    if (btb_table.empty()) {
        return nullptr;
    }

    const int pc_bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const int next_idx = (idx + 1) % positive_count(cfg.set_count);
    const bpu_addr_t tag = generate_tag(pc);
    const int bank_2b_count = get_bank_2b_count();
    const int lookup_base_offset_2b =
        ((pc >> cfg.tag_pc_shift) % bank_2b_count + bank_2b_count) %
        bank_2b_count;

    const auto sign_matches =
        [&branch_sign](const btb_entry_record_t& record,
                       int fetch_block_offset_2b) {
            if (!record.valid || record.cfi_sign.type != CFI_BRA ||
                branch_sign.type != CFI_BRA ||
                record.cfi_sign.compressed != branch_sign.compressed) {
                return false;
            }

            return fetch_block_offset_2b == branch_sign.offset;
        };

    for (int bank_offset = 0; bank_offset < positive_count(cfg.bank_count);
         ++bank_offset) {
        const int bank =
            (pc_bank + bank_offset) % positive_count(cfg.bank_count);
        const int set = bank < pc_bank ? next_idx : idx;
        const int bank_base_offset = bank_offset * bank_2b_count;

        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            btb_entry_t& entry = btb_table[way][bank][set];
            if (!entry.valid || entry.tag != tag ||
                entry.base_offset_2b != lookup_base_offset_2b) {
                continue;
            }

            if (sign_matches(entry.e1,
                             bank_base_offset + entry.e1.cfi_sign.offset)) {
                replacement.touch(replacement_states[bank][set],
                                  static_cast<std::size_t>(way));
                return &entry.e1;
            }

            if (sign_matches(entry.e2,
                             bank_base_offset + entry.e2.cfi_sign.offset)) {
                replacement.touch(replacement_states[bank][set],
                                  static_cast<std::size_t>(way));
                return &entry.e2;
            }
        }
    }

    return nullptr;
}

btb_commit_result_t
btb::update_branch_ctr(bpu_addr_t pc, const bpu_sign_t& branch_sign,
                       bool taken)
{
    btb_commit_result_t result;
    btb_entry_record_t* record = find_branch_record(pc, branch_sign);
    if (record == nullptr) {
        return result;
    }

    result.branch_ctr_updated = true;
    if (taken) {
        record->branch_ctr =
            std::min(record->branch_ctr + 1, bpu_cfg::btb_branch_ctr_max);
    } else {
        record->branch_ctr = std::max(record->branch_ctr - 1, 0);
    }
    result.branch_strongly_taken = is_strongly_taken(*record);
    return result;
}

btb_commit_result_t
btb::commit(const btb_commit_update_t& update)
{
    btb_commit_result_t result;
    if (!update.valid) {
        return result;
    }

    if (update.insert_entry) {
        const bpu_addr_t lookup_pc = update.lookup_pc_valid ?
            update.lookup_pc : update.pc;
        insert_or_update(update.pc, update.entry, lookup_pc);
    }

    if (update.update_branch_ctr) {
        const bpu_addr_t lookup_pc = update.lookup_pc_valid ?
            update.lookup_pc : update.pc;
        result = update_branch_ctr(lookup_pc, update.branch_sign,
                                   update.branch_taken);
    }

    return result;
}

ubtb_entry_t
btb::make_ubtb_entry(bpu_addr_t cfi_addr,
                     const btb_response_t& btb_response) const
{
    (void)cfi_addr;

    ubtb_entry_t ubtb_entry;
    ubtb_entry.valid =
        btb_response.valid && btb_response.taken && btb_response.ubtb_fillable;
    ubtb_entry.target = btb_response.target;
    ubtb_entry.cfi_sign = btb_response.sign;
    ubtb_entry.next_cfi_span_2b = btb_response.next_cfi_span_2b;
    return ubtb_entry;
}
