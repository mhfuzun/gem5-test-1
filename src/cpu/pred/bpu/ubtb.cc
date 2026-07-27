#include <algorithm>
#include <iterator>

#include "ubtb.hh"

namespace
{

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

ubtb::ubtb(ubtb_cfg cfg) {
    this->cfg = cfg;

    ubtb_entries.resize(std::max(0, cfg.way_count));
    replacement.reset(ubtb_entries.size());
    replacement_state = replacement.new_state();
    rebuild_index();
}

int ubtb::generate_tag(int pc) const {
    return (pc >> cfg.tag_pc_shift) & mask_bits(cfg.tag_width);
}

ubtb_entry_t* ubtb::lookup(int pc) {
    int tag = generate_tag(pc);

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

const ubtb_entry_t* ubtb::lookup(int pc) const {
    int tag = generate_tag(pc);

    auto it = tag_index.find(tag);
    if (it == tag_index.end()) {
        return nullptr;
    }

    const ubtb_entry_t& entry = ubtb_entries[it->second];
    return entry.valid && entry.tag == tag ? &entry : nullptr;
}

ubtb_response_t ubtb::predict(int pc) {
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
ubtb::insert_or_update(int pc, const ubtb_entry_t& new_entry)
{
    if (ubtb_entries.empty()) {
        return;
    }

    int tag = generate_tag(pc);
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
ubtb::invalidate(int pc)
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
