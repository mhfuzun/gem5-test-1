#include <algorithm>

#include "abtb.hh"

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

abtb::abtb(abtb_cfg cfg)
{
    this->cfg = cfg;

    table.resize(positive_count(cfg.bank_count));
    for (auto& bank : table) {
        bank.resize(positive_count(cfg.set_count));
    }
}

int
abtb::getBank2BCount() const
{
    const int banks = positive_count(cfg.bank_count);
    return std::max(1, bpu_cfg::fetch_block_2b_count / banks);
}

int
abtb::getBankIndex(bpu_addr_t addr) const
{
    const int banks = positive_count(cfg.bank_count);
    const bpu_addr_t bank_block =
        (addr >> cfg.tag_pc_shift) / getBank2BCount();
    return static_cast<int>(bank_block % static_cast<bpu_addr_t>(banks));
}

int
abtb::getSetIndex(bpu_addr_t addr) const
{
    const int sets = positive_count(cfg.set_count);
    const int banks = positive_count(cfg.bank_count);

    bpu_addr_t block = (addr >> cfg.tag_pc_shift) / getBank2BCount();
    block /= banks;
    return static_cast<int>(block % static_cast<bpu_addr_t>(sets));
}

int
abtb::getFetchBlockOffset2B(bpu_addr_t addr) const
{
    return static_cast<int>(
        (addr >> cfg.tag_pc_shift) %
        static_cast<bpu_addr_t>(bpu_cfg::fetch_block_2b_count));
}

bpu_sign_t
abtb::makeFetchBlockSign(const bpu_sign_t& bank_sign,
                         int fetch_block_offset_2b) const
{
    bpu_sign_t sign = bank_sign;
    sign.offset = fetch_block_offset_2b;
    return sign;
}

bpu_tag_t
abtb::getAbtbTag(bpu_addr_t addr) const
{
    const int banks = positive_count(cfg.bank_count);

    bpu_addr_t block = (addr >> cfg.tag_pc_shift) / getBank2BCount();
    block /= banks;
    return block & mask_bits(cfg.tag_width);
}

abtb::abtb_entry_t
abtb::lookup(bpu_addr_t addr) const
{
    if (table.empty()) {
        return {};
    }

    const int set = getSetIndex(addr);
    const bpu_tag_t tag = getAbtbTag(addr);
    const int banks = positive_count(cfg.bank_count);
    const int bank_2b_count = getBank2BCount();
    const int lookup_offset_2b = getFetchBlockOffset2B(addr);

    for (int bank = 0; bank < banks; ++bank) {
        const abtb_entry_t& entry = table[bank][set];
        if (!entry.valid || entry.tag != tag) {
            continue;
        }

        const int bank_cfi_offset_2b = std::max(0, entry.cfi_entry.offset);
        const int fetch_block_offset_2b =
            bank * bank_2b_count + bank_cfi_offset_2b;
        if (fetch_block_offset_2b < lookup_offset_2b ||
            fetch_block_offset_2b >= bpu_cfg::fetch_block_2b_count) {
            continue;
        }

        abtb_entry_t response = entry;
        response.cfi_entry =
            makeFetchBlockSign(entry.cfi_entry, fetch_block_offset_2b);
        return response;
    }

    return {};
}

btb_response_t
abtb::predict(bpu_addr_t addr) const
{
    const abtb_entry_t entry = lookup(addr);
    btb_response_t response;
    if (!entry.valid) {
        return response;
    }

    response.valid = true;
    response.taken = true;
    response.target = entry.target_addr;
    response.next_cfi_addr = entry.next_cfi_addr;
    response.next_cfi_span_2b = entry.next_cfi_span_2b;
    response.sign = entry.cfi_entry;
    response.ubtb_fillable = true;
    response.branch_strongly_taken = true;
    return response;
}

void
abtb::insert_or_update(bpu_addr_t addr, const abtb_entry_t& new_entry)
{
    if (table.empty()) {
        return;
    }

    const int bank = getBankIndex(addr);
    const int set = getSetIndex(addr);

    abtb_entry_t& entry = table[bank][set];
    entry = new_entry;
    entry.valid = true;
    entry.tag = getAbtbTag(addr);
    entry.cfi_entry.offset =
        std::max(0, entry.cfi_entry.offset) % getBank2BCount();
}

void
abtb::insert_or_update(bpu_addr_t addr, bpu_addr_t target_addr,
                       const bpu_sign_t& cfi_entry, int next_cfi_span_2b,
                       bpu_addr_t next_cfi_addr)
{
    abtb_entry_t entry;
    entry.target_addr = target_addr;
    entry.next_cfi_addr = next_cfi_addr;
    entry.next_cfi_span_2b = next_cfi_span_2b;
    entry.cfi_entry = cfi_entry;
    insert_or_update(addr, entry);
}

void
abtb::insert_or_update(bpu_addr_t addr, const btb_response_t& response)
{
    if (!response.valid || !response.taken) {
        return;
    }

    insert_or_update(addr, response.target, response.sign,
                     response.next_cfi_span_2b, response.next_cfi_addr);
}

void
abtb::invalidate(bpu_addr_t addr)
{
    if (table.empty()) {
        return;
    }

    const int bank = getBankIndex(addr);
    const int set = getSetIndex(addr);
    abtb_entry_t& entry = table[bank][set];

    if (entry.valid && entry.tag == getAbtbTag(addr)) {
        entry.valid = false;
    }
}

void
abtb::clear()
{
    for (auto& bank : table) {
        for (auto& entry : bank) {
            entry = {};
        }
    }
}

void
abtb::commit(bpu_addr_t addr, bpu_addr_t target_addr,
             const bpu_sign_t& cfi_entry, int next_cfi_span_2b,
             bpu_addr_t next_cfi_addr)
{
    insert_or_update(addr, target_addr, cfi_entry, next_cfi_span_2b,
                     next_cfi_addr);
}

void
abtb::commit()
{
    // Kept for the original placeholder API; real updates use the overload.
}
