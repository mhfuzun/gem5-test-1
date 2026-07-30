#include <algorithm>
#include <iterator>

#include "ubtb.hh"

namespace
{

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

ubtb::ubtb(ubtb_cfg cfg) {
    this->cfg = cfg;

    ubtb_entries.resize(std::max(0, cfg.way_count));
    replacement.reset(ubtb_entries.size());
    replacement_state = replacement.new_state();
    rebuild_index();
}

bpu_addr_t ubtb::generate_tag(bpu_addr_t pc) const {
    return (pc >> cfg.tag_pc_shift) & mask_bits(cfg.tag_width);
}

ubtb_entry_t* ubtb::lookup(bpu_addr_t pc) {
    bpu_addr_t tag = generate_tag(pc);

    auto it = tag_index.find(tag);
    if (it == tag_index.end()) {
        return nullptr;
    }

    const std::size_t idx = it->second;
    ubtb_entry_t& entry = ubtb_entries[idx];
    if (entry.valid && entry.tag == tag) {
        replacement.touch(replacement_state, idx);
    }
    return entry.valid && entry.tag == tag ? &entry : nullptr;
}

const ubtb_entry_t* ubtb::lookup(bpu_addr_t pc) const {
    bpu_addr_t tag = generate_tag(pc);

    auto it = tag_index.find(tag);
    if (it == tag_index.end()) {
        return nullptr;
    }

    const ubtb_entry_t& entry = ubtb_entries[it->second];
    return entry.valid && entry.tag == tag ? &entry : nullptr;
}

ubtb_response_t ubtb::predict(bpu_addr_t pc) {
    const ubtb_entry_t* entry = lookup(pc);

    if (entry != nullptr) {
        ubtb_response_t response;
        response.valid = true;
        response.taken = entry->valid;
        response.target = entry->target;
        response.next_cfi_span_2b = entry->next_cfi_span_2b;
        response.sign = entry->cfi_sign;
        return response;
    }

    return {};
}

void
ubtb::insert_or_update(bpu_addr_t pc, const ubtb_entry_t& new_entry)
{
    if (ubtb_entries.empty()) {
        return;
    }

    bpu_addr_t tag = generate_tag(pc);
    auto it = tag_index.find(tag);
    std::size_t idx = 0;

    if (it != tag_index.end()) {
        idx = it->second;
    } else {
        const auto invalid = std::find_if(
            ubtb_entries.begin(), ubtb_entries.end(),
            [](const ubtb_entry_t& entry) { return !entry.valid; });
        if (invalid != ubtb_entries.end()) {
            idx = static_cast<std::size_t>(
                std::distance(ubtb_entries.begin(), invalid));
            replacement.touch(replacement_state, idx);
        } else {
            idx = replacement.get_lru_and_touch(replacement_state);
        }
    }

    if (ubtb_entries[idx].valid) {
        tag_index.erase(ubtb_entries[idx].tag);
    }

    ubtb_entries[idx] = new_entry;
    ubtb_entries[idx].valid = true;
    ubtb_entries[idx].tag = tag;
    tag_index[tag] = idx;
    replacement.touch(replacement_state, idx);
}

void
ubtb::invalidate(bpu_addr_t pc)
{
    ubtb_entry_t* entry = lookup(pc);
    if (entry == nullptr) {
        return;
    }

    tag_index.erase(entry->tag);
    entry->valid = false;
}

void
ubtb::rebuild_index()
{
    tag_index.clear();

    for (std::size_t i = 0; i < ubtb_entries.size(); ++i) {
        const auto& entry = ubtb_entries[i];
        if (entry.valid) {
            tag_index[entry.tag] = i;
        }
    }
}
