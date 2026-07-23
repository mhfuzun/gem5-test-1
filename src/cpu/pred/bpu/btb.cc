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

int
mask_bits(int width)
{
    if (width <= 0) {
        return 0;
    }

    if (width >= static_cast<int>(sizeof(int) * 8)) {
        return ~0;
    }

    return (1 << width) - 1;
}

} // namespace

btb::btb(btb_cfg cfg)
{
    this->cfg = cfg;

    const int ways = positive_count(cfg.way_count);
    const int banks = positive_count(cfg.bank_count);
    const int sets = positive_count(cfg.set_count);

    btb_table.resize(ways);
    for (auto& way : btb_table) {
        way.resize(banks);
        for (auto& bank : way) {
            bank.resize(sets);
        }
    }
}

int
btb::get_bank_index(int pc) const
{
    const int banks = positive_count(cfg.bank_count);
    return ((pc >> cfg.tag_pc_shift) % banks + banks) % banks;
}

int
btb::generate_index(int pc) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);
    int block = pc >> cfg.tag_pc_shift;
    block /= banks;
    return (block % sets + sets) % sets;
}

int
btb::generate_tag(int pc) const
{
    return (pc >> cfg.tag_pc_shift) & mask_bits(cfg.tag_width);
}

std::vector<btb::btb_lookup_bank_t>
btb::lookup(int pc) const
{
    std::vector<btb_lookup_bank_t> result;
    result.reserve(positive_count(cfg.bank_count));

    const int pc_bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const int next_idx = (idx + 1) % positive_count(cfg.set_count);
    const int tag = generate_tag(pc);

    for (int bank_offset = 0; bank_offset < positive_count(cfg.bank_count);
         ++bank_offset) {
        const int bank =
            (pc_bank + bank_offset) % positive_count(cfg.bank_count);
        const int set = bank < pc_bank ? next_idx : idx;
        btb_lookup_bank_t bank_result;
        bank_result.bank = bank;

        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            const btb_entry_t& entry = btb_table[way][bank][set];
            if (!entry.valid || entry.tag != tag) {
                continue;
            }

            if (entry.e1.valid) {
                bank_result.records.push_back(&entry.e1);
            }

            if (entry.e2.valid) {
                bank_result.records.push_back(&entry.e2);
            }
        }

        result.push_back(bank_result);
    }

    return result;
}

void
btb::predict(btb_response_t& response, int pc,
             tage_response_t tage_response, tt_bank_response_t tt_response,
             ras_response_t ras_response)
{
    response = predict(pc, tage_response, tt_response, ras_response);
}

btb_response_t
btb::predict(int pc, tage_response_t tage_response,
             tt_bank_response_t tt_response,
             ras_response_t ras_response)
{
    const std::vector<btb_lookup_bank_t> bank_entries = lookup(pc);
    int branch_count = 0;
    btb_response_t fallthrough_response;

    for (std::size_t bank_offset = 0; bank_offset < bank_entries.size();
         ++bank_offset) {
        const btb_lookup_bank_t& bank_entry = bank_entries[bank_offset];

        for (const btb_entry_record_t* entry : bank_entry.records) {
            if (entry == nullptr || !entry->valid) {
                continue;
            }

            if (entry->cfi_sign.type == CFI_BRA) {
                const bool taken =
                    branch_count < 2 &&
                    tage_response.predictions[branch_count];
                branch_count++;

                if (taken) {
                    btb_response_t response;
                    response.valid = true;
                    response.taken = true;
                    response.target = entry->target;
                    response.next_cfi_span_2b =
                        entry->taken_next_cfi_span_2b;
                    response.sign = entry->cfi_sign;
                    return response;
                }

                fallthrough_response.valid = true;
                fallthrough_response.taken = false;
                fallthrough_response.next_cfi_span_2b =
                    entry->fallthrough_next_cfi_span_2b;
                fallthrough_response.sign = entry->cfi_sign;
            } else if (entry->cfi_sign.type == CFI_JAL) {
                btb_response_t response;
                response.valid = true;
                response.taken = true;
                response.target = entry->target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = entry->cfi_sign;
                return response;
            } else if (entry->cfi_sign.type == CFI_JALR_CALL) {
                const tt_response_t bank_tt =
                    bank_offset < tt_response.banks.size() ?
                    tt_response.banks[bank_offset] : tt_response_t{};

                btb_response_t response;
                response.valid = true;
                response.taken = bank_tt.hit;
                response.target = bank_tt.target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = entry->cfi_sign;
                response.tt_hit = bank_tt.hit;
                return response;
            } else if (entry->cfi_sign.type == CFI_JALR_RET) {
                btb_response_t response;
                response.valid = true;
                response.taken = ras_response.valid;
                response.target = ras_response.target;
                response.next_cfi_span_2b =
                    entry->taken_next_cfi_span_2b;
                response.sign = entry->cfi_sign;
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
btb::insert_or_update(int pc, const btb_entry_t& new_entry)
{
    if (btb_table.empty()) {
        return;
    }

    const int bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    btb_entry_t& entry = btb_table[0][bank][idx];

    entry = new_entry;
    entry.valid = true;
    entry.tag = generate_tag(pc);
}
