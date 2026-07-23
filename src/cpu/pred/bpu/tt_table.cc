#include <algorithm>

#include "tt_table.hh"

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

tt_table::tt_table(tt_cfg cfg)
{
    this->cfg = cfg;

    const int ways = positive_count(cfg.way_count);
    const int banks = positive_count(cfg.bank_count);
    const int sets = positive_count(cfg.set_count);

    tt.resize(ways);
    for (auto& way : tt) {
        way.resize(banks);
        for (auto& bank : way) {
            bank.resize(sets);
        }
    }
}

int
tt_table::get_bank_index(int pc) const
{
    const int banks = positive_count(cfg.bank_count);
    return ((pc >> cfg.tag_pc_shift) % banks + banks) % banks;
}

int
tt_table::generate_index(int pc) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);
    int block = pc >> cfg.tag_pc_shift;
    block /= banks;
    return (block % sets + sets) % sets;
}

int
tt_table::generate_tag(int pc) const
{
    return (pc >> cfg.tag_pc_shift) & mask_bits(cfg.tag_width);
}

tt_bank_response_t
tt_table::lookup(int pc) const
{
    tt_bank_response_t response;
    response.banks.resize(positive_count(cfg.bank_count));

    const int pc_bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const int next_idx = (idx + 1) % positive_count(cfg.set_count);
    const int tag = generate_tag(pc);

    for (int bank_offset = 0; bank_offset < positive_count(cfg.bank_count);
         ++bank_offset) {
        const int bank =
            (pc_bank + bank_offset) % positive_count(cfg.bank_count);
        const int set = bank < pc_bank ? next_idx : idx;
        tt_response_t& bank_response = response.banks[bank_offset];

        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            const tt_entry_t& entry = tt[way][bank][set];
            if (!entry.valid || entry.tag != tag) {
                continue;
            }

            bank_response.hit = true;
            bank_response.target = entry.target;
            break;
        }
    }

    return response;
}

void
tt_table::insert_or_update(int pc, int target)
{
    if (tt.empty()) {
        return;
    }

    const int bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    tt_entry_t& entry = tt[0][bank][idx];

    entry.valid = true;
    entry.tag = generate_tag(pc);
    entry.target = target;
}
