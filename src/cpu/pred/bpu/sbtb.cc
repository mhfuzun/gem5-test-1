#include <algorithm>
#include <array>
#include <iterator>

#include "sbtb.hh"

namespace
{

int
positive_count(int value)
{
    return std::max(1, value);
}

bpu_tag_t
mask_bits(int width)
{
    if (width <= 0) {
        return 0;
    }

    if (width >= static_cast<int>(sizeof(bpu_tag_t) * 8)) {
        return ~bpu_tag_t{0};
    }

    return (bpu_tag_t{1} << width) - 1;
}

} // namespace

sbtb::sbtb(sbtb_cfg cfg)
{
    this->cfg = cfg;

    const int banks = positive_count(cfg.bank_count);
    const int sets = positive_count(cfg.set_count);
    const int ways = positive_count(cfg.way_count);

    replacement.reset(ways);

    table.resize(banks);
    tag_index.resize(banks);
    free_ways.resize(banks);
    replacement_states.resize(banks);
    for (int bank = 0; bank < banks; ++bank) {
        table[bank].resize(sets);
        tag_index[bank].resize(sets);
        free_ways[bank].resize(sets);
        replacement_states[bank].resize(sets);
        for (int set = 0; set < sets; ++set) {
            table[bank][set].resize(ways);
            free_ways[bank][set].reserve(ways);
            for (int way = ways - 1; way >= 0; --way) {
                free_ways[bank][set].push_back(
                    static_cast<std::size_t>(way));
            }
            replacement_states[bank][set] = replacement.new_state();
        }
    }
}

int
sbtb::get_bank_2b_count() const
{
    const int banks = positive_count(cfg.bank_count);
    return std::max(1, bpu_cfg::fetch_block_2b_count / banks);
}

int
sbtb::get_bank_index(bpu_addr_t addr) const
{
    const int banks = positive_count(cfg.bank_count);
    const bpu_addr_t bank_block =
        (addr >> cfg.tag_pc_shift) / get_bank_2b_count();
    return static_cast<int>(bank_block % static_cast<bpu_addr_t>(banks));
}

int
sbtb::generate_index(bpu_addr_t addr) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);

    bpu_addr_t block = (addr >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return static_cast<int>(block % static_cast<bpu_addr_t>(sets));
}

bpu_tag_t
sbtb::generate_tag(bpu_addr_t addr) const
{
    const int banks = positive_count(cfg.bank_count);

    bpu_addr_t block = (addr >> cfg.tag_pc_shift) / get_bank_2b_count();
    block /= banks;
    return block & mask_bits(cfg.tag_width);
}

int
sbtb::get_bank_local_offset_2b(bpu_addr_t addr) const
{
    const int bank_2b_count = get_bank_2b_count();
    return static_cast<int>(
        (addr >> cfg.tag_pc_shift) %
        static_cast<bpu_addr_t>(bank_2b_count));
}

bool
sbtb::is_taken(const sbtb_entry_record_t& record) const
{
    if (!record.valid) {
        return false;
    }

    if (record.cfi_entry.type == CFI_JAL) {
        return true;
    }

    if (record.cfi_entry.type == CFI_JALR_CALL) {
        return record.target_addr != 0;
    }

    if (record.cfi_entry.type == CFI_BRA) {
        return record.ctr >= 2;
    }

    return false;
}

bool
sbtb::is_after_lookup(const sbtb_entry_record_t& record, int bank_offset,
                      int lookup_bank_offset_2b) const
{
    if (!record.valid) {
        return false;
    }

    const int record_offset_2b = std::max(0, record.cfi_entry.offset);
    if (record_offset_2b >= get_bank_2b_count()) {
        return false;
    }

    return bank_offset > 0 || record_offset_2b >= lookup_bank_offset_2b;
}

bool
sbtb::same_cfi(const sbtb_entry_record_t& record,
               const bpu_sign_t& sign) const
{
    return record.valid &&
        record.cfi_entry.offset == sign.offset &&
        record.cfi_entry.type == sign.type &&
        record.cfi_entry.compressed == sign.compressed &&
        record.cfi_entry.is_call == sign.is_call;
}

sbtb::sbtb_entry_t*
sbtb::lookup_entry(int bank, int set, bpu_tag_t tag)
{
    if (bank < 0 || set < 0 ||
        static_cast<std::size_t>(bank) >= tag_index.size() ||
        static_cast<std::size_t>(set) >= tag_index[bank].size()) {
        return nullptr;
    }

    auto it = tag_index[bank][set].find(tag);
    if (it == tag_index[bank][set].end()) {
        return nullptr;
    }

    const std::size_t way = it->second;
    if (way >= table[bank][set].size()) {
        return nullptr;
    }

    sbtb_entry_t& entry = table[bank][set][way];
    if (!entry.valid || entry.tag != tag) {
        tag_index[bank][set].erase(it);
        return nullptr;
    }

    replacement.touch(replacement_states[bank][set], way);
    return &entry;
}

const sbtb::sbtb_entry_t*
sbtb::lookup_entry(int bank, int set, bpu_tag_t tag) const
{
    if (bank < 0 || set < 0 ||
        static_cast<std::size_t>(bank) >= tag_index.size() ||
        static_cast<std::size_t>(set) >= tag_index[bank].size()) {
        return nullptr;
    }

    auto it = tag_index[bank][set].find(tag);
    if (it == tag_index[bank][set].end()) {
        return nullptr;
    }

    const std::size_t way = it->second;
    if (way >= table[bank][set].size()) {
        return nullptr;
    }

    const sbtb_entry_t& entry = table[bank][set][way];
    return entry.valid && entry.tag == tag ? &entry : nullptr;
}

btb_response_t
sbtb::predict(bpu_addr_t cfi_addr)
{
    btb_response_t response;
    if (table.empty()) {
        return response;
    }

    const int banks = positive_count(cfg.bank_count);
    const int bank_2b_count = get_bank_2b_count();
    const int lookup_bank_offset_2b = get_bank_local_offset_2b(cfi_addr);

    const bpu_addr_t first_bank_addr =
        cfi_addr -
        static_cast<bpu_addr_t>(lookup_bank_offset_2b) * 2;

    for (int bank_offset = 0; bank_offset < banks; ++bank_offset) {
        const bpu_addr_t bank_addr =
            first_bank_addr +
            static_cast<bpu_addr_t>(bank_offset * bank_2b_count) * 2;
        const int bank = get_bank_index(bank_addr);
        const int set = generate_index(bank_addr);
        const bpu_tag_t tag = generate_tag(bank_addr);

        const sbtb_entry_t* entry = lookup_entry(bank, set, tag);
        if (entry == nullptr) {
            continue;
        }

        std::array<const sbtb_entry_record_t*, 2> records = {
            &entry->e1, &entry->e2
        };
        if (records[1]->valid &&
            (!records[0]->valid ||
             records[1]->cfi_entry.offset < records[0]->cfi_entry.offset)) {
            std::swap(records[0], records[1]);
        }

        for (const sbtb_entry_record_t* record : records) {
            if (record == nullptr ||
                !is_after_lookup(*record, bank_offset,
                                 lookup_bank_offset_2b) ||
                !is_taken(*record)) {
                continue;
            }

            const int relative_offset_2b =
                bank_offset * bank_2b_count +
                std::max(0, record->cfi_entry.offset) -
                lookup_bank_offset_2b;
            if (relative_offset_2b < 0 ||
                relative_offset_2b >= bpu_cfg::fetch_block_2b_count) {
                continue;
            }

            response.valid = true;
            response.taken = true;
            response.target = record->target_addr;
            response.next_cfi_addr = record->next_cfi_addr;
            response.next_cfi_span_2b = record->next_cfi_span_2b;
            response.sign = record->cfi_entry;
            response.sign.offset = relative_offset_2b;
            response.tt_hit = record->cfi_entry.type == CFI_JALR_CALL;
            response.branch_strongly_taken =
                record->ctr >= bpu_cfg::btb_branch_ctr_strong_taken;
            return response;
        }
    }

    return response;
}

void
sbtb::insert_or_update(bpu_addr_t cfi_addr, const sbtb_entry_t& new_entry)
{
    if (table.empty()) {
        return;
    }

    const int bank = get_bank_index(cfi_addr);
    const int set = generate_index(cfi_addr);
    const bpu_tag_t tag = generate_tag(cfi_addr);

    sbtb_entry_t* entry = lookup_entry(bank, set, tag);
    std::size_t way = 0;
    if (entry == nullptr) {
        if (!free_ways[bank][set].empty()) {
            way = free_ways[bank][set].back();
            free_ways[bank][set].pop_back();
        } else {
            way = replacement.get_lru_and_touch(replacement_states[bank][set]);
            if (table[bank][set][way].valid) {
                tag_index[bank][set].erase(table[bank][set][way].tag);
            }
        }

        entry = &table[bank][set][way];
    }

    *entry = new_entry;
    entry->valid = true;
    entry->tag = tag;
    entry->e1.cfi_entry.offset =
        std::max(0, entry->e1.cfi_entry.offset) % get_bank_2b_count();
    entry->e2.cfi_entry.offset =
        std::max(0, entry->e2.cfi_entry.offset) % get_bank_2b_count();

    way = static_cast<std::size_t>(entry - table[bank][set].data());
    tag_index[bank][set][tag] = way;
    replacement.touch(replacement_states[bank][set], way);
}

void
sbtb::update(bpu_addr_t cfi_addr, bpu_addr_t target_addr,
             const bpu_sign_t& cfi_entry, bool taken, int next_cfi_span_2b,
             bpu_addr_t next_cfi_addr)
{
    if (table.empty()) {
        return;
    }

    bpu_sign_t bank_sign = cfi_entry;
    bank_sign.offset = get_bank_local_offset_2b(cfi_addr);

    const int bank = get_bank_index(cfi_addr);
    const int set = generate_index(cfi_addr);
    const bpu_tag_t tag = generate_tag(cfi_addr);

    sbtb_entry_t* entry = lookup_entry(bank, set, tag);
    if (entry == nullptr) {
        sbtb_entry_t new_entry;
        new_entry.e1.valid = true;
        new_entry.e1.ctr = taken ?
            bpu_cfg::btb_branch_ctr_strong_taken : 0;
        new_entry.e1.target_addr = target_addr;
        new_entry.e1.next_cfi_addr = next_cfi_addr;
        new_entry.e1.next_cfi_span_2b = next_cfi_span_2b;
        new_entry.e1.cfi_entry = bank_sign;
        insert_or_update(cfi_addr, new_entry);
        return;
    }

    sbtb_entry_record_t* record = nullptr;
    if (same_cfi(entry->e1, bank_sign)) {
        record = &entry->e1;
    } else if (same_cfi(entry->e2, bank_sign)) {
        record = &entry->e2;
    } else if (!entry->e1.valid) {
        record = &entry->e1;
    } else if (!entry->e2.valid) {
        record = &entry->e2;
    } else if (bank_sign.offset < entry->e1.cfi_entry.offset ||
               entry->e2.cfi_entry.offset < entry->e1.cfi_entry.offset) {
        record = &entry->e1;
    } else {
        record = &entry->e2;
    }

    if (!record->valid || !same_cfi(*record, bank_sign)) {
        record->valid = true;
        record->ctr = taken ?
            bpu_cfg::btb_branch_ctr_strong_taken : 0;
        record->cfi_entry = bank_sign;
    } else if (taken) {
        record->ctr = std::min(record->ctr + 1, bpu_cfg::btb_branch_ctr_max);
    } else {
        record->ctr = std::max(record->ctr - 1, 0);
    }

    record->target_addr = target_addr;
    record->next_cfi_addr = next_cfi_addr;
    record->next_cfi_span_2b = next_cfi_span_2b;
}

void
sbtb::update(bpu_addr_t cfi_addr, const btb_response_t& response, bool taken)
{
    if (!response.valid) {
        return;
    }

    update(cfi_addr, response.target, response.sign, taken,
           response.next_cfi_span_2b, response.next_cfi_addr);
}

void
sbtb::invalidate(bpu_addr_t cfi_addr)
{
    if (table.empty()) {
        return;
    }

    const int bank = get_bank_index(cfi_addr);
    const int set = generate_index(cfi_addr);
    const bpu_tag_t tag = generate_tag(cfi_addr);
    sbtb_entry_t* entry = lookup_entry(bank, set, tag);
    if (entry == nullptr) {
        return;
    }

    tag_index[bank][set].erase(tag);
    const std::size_t way =
        static_cast<std::size_t>(entry - table[bank][set].data());
    *entry = {};
    free_ways[bank][set].push_back(way);
}

void
sbtb::clear()
{
    for (auto& bank : table) {
        for (auto& set : bank) {
            for (auto& entry : set) {
                entry = {};
            }
        }
    }

    for (auto& bank : tag_index) {
        for (auto& set : bank) {
            set.clear();
        }
    }

    for (std::size_t bank = 0; bank < free_ways.size(); ++bank) {
        for (std::size_t set = 0; set < free_ways[bank].size(); ++set) {
            free_ways[bank][set].clear();
            const std::size_t ways = table[bank][set].size();
            free_ways[bank][set].reserve(ways);
            for (std::size_t way = ways; way > 0; --way) {
                free_ways[bank][set].push_back(way - 1);
            }
        }
    }

    for (auto& bank : replacement_states) {
        for (auto& set : bank) {
            set = replacement.new_state();
        }
    }
}

void
sbtb::rebuild_index()
{
    for (auto& bank : tag_index) {
        for (auto& set : bank) {
            set.clear();
        }
    }

    for (auto& bank : free_ways) {
        for (auto& set : bank) {
            set.clear();
        }
    }

    for (std::size_t bank = 0; bank < table.size(); ++bank) {
        for (std::size_t set = 0; set < table[bank].size(); ++set) {
            for (std::size_t way = 0; way < table[bank][set].size(); ++way) {
                const sbtb_entry_t& entry = table[bank][set][way];
                if (entry.valid) {
                    tag_index[bank][set][entry.tag] = way;
                } else {
                    free_ways[bank][set].push_back(way);
                }
            }
        }
    }
}
