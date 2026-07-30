#include <algorithm>

#include "tt_table.hh"

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

tt_table::tt_table(tt_cfg cfg)
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

    tt.resize(ways);
    for (auto& way : tt) {
        way.resize(banks);
        for (auto& bank : way) {
            bank.resize(sets);
        }
    }
}

int
tt_table::get_bank_2b_count() const
{
    const int banks = positive_count(cfg.bank_count);
    return std::max(1, bpu_cfg::fetch_block_2b_count / banks);
}

int
tt_table::get_bank_index(bpu_addr_t pc) const
{
    const int banks = positive_count(cfg.bank_count);
    const bpu_addr_t bank_block =
        (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    return static_cast<int>(bank_block % static_cast<bpu_addr_t>(banks));
}

int
tt_table::generate_index(bpu_addr_t pc) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);
    bpu_addr_t block = (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return static_cast<int>(block % static_cast<bpu_addr_t>(sets));
}

bpu_addr_t
tt_table::generate_tag(bpu_addr_t pc) const
{
    const int banks = positive_count(cfg.bank_count);
    bpu_addr_t block = (pc >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return block & mask_bits(cfg.tag_width);
}

tt_bank_response_t
tt_table::lookup(bpu_addr_t pc)
{
    tt_bank_response_t response;
    response.banks.resize(positive_count(cfg.bank_count));

    const int pc_bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const int next_idx = (idx + 1) % positive_count(cfg.set_count);
    const bpu_addr_t tag = generate_tag(pc);

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
            replacement.touch(replacement_states[bank][set],
                              static_cast<std::size_t>(way));
            break;
        }
    }

    return response;
}

void
tt_table::insert_or_update(bpu_addr_t pc, bpu_addr_t target)
{
    insert_or_update(pc, target, pc);
}

void
tt_table::insert_or_update(bpu_addr_t pc, bpu_addr_t target, bpu_addr_t tag_pc)
{
    if (tt.empty()) {
        return;
    }

    const int bank = get_bank_index(pc);
    const int idx = generate_index(pc);
    const bpu_addr_t tag = generate_tag(tag_pc);

    tt_entry_t* entry = nullptr;
    std::size_t entry_way = 0;
    for (int way = 0; way < positive_count(cfg.way_count); ++way) {
        tt_entry_t& candidate = tt[way][bank][idx];
        if (candidate.valid && candidate.tag == tag) {
            entry = &candidate;
            entry_way = static_cast<std::size_t>(way);
            break;
        }
    }

    if (entry == nullptr) {
        for (int way = 0; way < positive_count(cfg.way_count); ++way) {
            tt_entry_t& candidate = tt[way][bank][idx];
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
        entry = &tt[entry_way][bank][idx];
    }

    entry->valid = true;
    entry->tag = tag;
    entry->target = target;
    replacement.touch(plru_state, entry_way);
}

void
tt_table::commit(const tt_commit_update_t& update)
{
    if (!update.valid) {
        return;
    }

    const bpu_addr_t lookup_pc =
        update.lookup_pc_valid ? update.lookup_pc : update.pc;
    insert_or_update(update.pc, update.target, lookup_pc);
}
