#include "cpu/o3/cfi_tracer.hh"

#include <algorithm>

cfi_tracer::cfi_tracer(int depth, int bank_count)
    : depth(std::max(1, depth)),
      bank_count(std::max(1, bank_count))
{
}

int
cfi_tracer::get_bank_2b_count() const
{
    return std::max(1, bpu_cfg::fetch_block_2b_count / bank_count);
}

int
cfi_tracer::get_bank_slot(const bpu_sign_t& sign) const
{
    const int offset = std::max(0, sign.offset);
    return std::min(bank_count - 1, offset / get_bank_2b_count());
}

bpu_addr_t
cfi_tracer::get_bank_pc(bpu_addr_t fetch_block_addr, int bank_slot) const
{
    return fetch_block_addr + bank_slot * get_bank_2b_count() * 2;
}

bpu_sign_t
cfi_tracer::make_btb_sign(const bpu_sign_t& fetch_block_sign) const
{
    bpu_sign_t btb_sign = fetch_block_sign;
    const int bank_slot = get_bank_slot(fetch_block_sign);
    btb_sign.offset =
        std::max(0, fetch_block_sign.offset) -
        bank_slot * get_bank_2b_count();
    return btb_sign;
}

void
cfi_tracer::add_fetch_block(ThreadID tid, bpu_addr_t fetch_block_addr,
                            InstSeqNum first_seq, InstSeqNum last_seq,
                            const std::vector<cfi_point_t>& cfi_points)
{
    cfi_tracer_entry_t entry;
    entry.valid = true;
    entry.tid = tid;
    entry.first_seq = first_seq;
    entry.last_seq = last_seq;
    entry.fetch_block_addr = fetch_block_addr;
    entry.cfi_points = cfi_points;

    cfi_tracer_entries.push_back(entry);
    while (static_cast<int>(cfi_tracer_entries.size()) > depth) {
        cfi_tracer_entries.erase(cfi_tracer_entries.begin());
    }
}

cfi_tracer::check_result_t
cfi_tracer::check(ThreadID tid, bpu_addr_t fetch_block_addr,
                  const std::vector<bpu_sign_t>& bpu_sign_vector) const
{
    check_result_t result;
    const cfi_tracer_entry_t* entry = lookup(tid, fetch_block_addr);
    if (entry == nullptr) {
        result.mismatch = true;
        return result;
    }

    const std::size_t compare_count =
        std::min(entry->cfi_points.size(), bpu_sign_vector.size());
    std::size_t mismatch_idx = compare_count;

    for (std::size_t i = 0; i < compare_count; ++i) {
        const bpu_sign_t& actual = entry->cfi_points[i].sign;
        const bpu_sign_t& predicted = bpu_sign_vector[i];
        if (actual.offset != predicted.offset ||
            actual.type != predicted.type ||
            actual.compressed != predicted.compressed ||
            actual.is_call != predicted.is_call) {
            mismatch_idx = i;
            break;
        }
    }

    if (mismatch_idx == compare_count &&
        entry->cfi_points.size() == bpu_sign_vector.size()) {
        return result;
    }

    result.mismatch = true;
    if (mismatch_idx >= entry->cfi_points.size()) {
        return result;
    }

    const cfi_point_t& actual = entry->cfi_points[mismatch_idx];
    result.sign = actual.sign;

    if (actual.sign.type == CFI_JALR_CALL ||
        actual.sign.type == CFI_JALR_RET) {
        result.wait_for_backend = true;
        return result;
    }

    if (actual.sign.type == CFI_JAL || actual.sign.type == CFI_BRA) {
        result.needs_redirect = true;
        result.redirect_target = actual.target;
    }

    return result;
}

const cfi_tracer::cfi_tracer_entry_t*
cfi_tracer::lookup(ThreadID tid, bpu_addr_t fetch_block_addr) const
{
    for (auto it = cfi_tracer_entries.rbegin();
         it != cfi_tracer_entries.rend(); ++it) {
        if (it->valid && it->tid == tid &&
            it->fetch_block_addr == fetch_block_addr) {
            return &*it;
        }
    }

    return nullptr;
}

btb_entry_t
cfi_tracer::make_btb_entry(ThreadID tid, bpu_addr_t fetch_block_addr) const
{
    const std::vector<btb_commit_update_t> updates =
        make_btb_commit_updates(tid, fetch_block_addr);
    if (updates.empty()) {
        return {};
    }

    return updates.front().entry;
}

std::vector<btb_commit_update_t>
cfi_tracer::make_btb_commit_updates(ThreadID tid,
                                    bpu_addr_t fetch_block_addr) const
{
    std::vector<btb_commit_update_t> updates;
    const cfi_tracer_entry_t* entry = lookup(tid, fetch_block_addr);
    if (entry == nullptr || entry->cfi_points.empty()) {
        return updates;
    }

    auto fill_record = [](btb_entry_record_t& record,
                          const cfi_point_t& cfi_point) {
        record.valid = true;
        record.target = cfi_point.target;
        record.cfi_sign = cfi_point.sign;
        record.taken_next_cfi_addr = cfi_point.next_cfi_addr;
        record.fallthrough_next_cfi_addr = cfi_point.next_cfi_addr;
        record.taken_next_cfi_span_2b = cfi_point.next_cfi_span_2b;
        record.fallthrough_next_cfi_span_2b = cfi_point.next_cfi_span_2b;
    };

    std::vector<btb_entry_t> bank_entries(bank_count);
    std::vector<bool> bank_touched(bank_count, false);

    for (const cfi_point_t& cfi_point : entry->cfi_points) {
        const int bank_slot = get_bank_slot(cfi_point.sign);
        btb_entry_t& btb_entry = bank_entries[bank_slot];
        cfi_point_t bank_point = cfi_point;
        bank_point.sign = make_btb_sign(cfi_point.sign);

        if (!btb_entry.e1.valid) {
            fill_record(btb_entry.e1, bank_point);
        } else if (!btb_entry.e2.valid) {
            fill_record(btb_entry.e2, bank_point);
        } else {
            continue;
        }

        btb_entry.valid = true;
        bank_touched[bank_slot] = true;
    }

    for (int bank_slot = 0; bank_slot < bank_count; ++bank_slot) {
        if (!bank_touched[bank_slot]) {
            continue;
        }

        btb_commit_update_t update;
        update.valid = true;
        update.insert_entry = true;
        update.pc = get_bank_pc(fetch_block_addr, bank_slot);
        update.lookup_pc_valid = true;
        update.lookup_pc = fetch_block_addr;
        update.ubtb_pc_valid = true;
        update.ubtb_pc = fetch_block_addr;
        update.entry = bank_entries[bank_slot];
        updates.push_back(update);
    }

    return updates;
}

void
cfi_tracer::squash_after(ThreadID tid, InstSeqNum seq_num)
{
    cfi_tracer_entries.erase(
        std::remove_if(cfi_tracer_entries.begin(), cfi_tracer_entries.end(),
            [tid, seq_num](const cfi_tracer_entry_t& entry) {
                return entry.valid && entry.tid == tid &&
                    entry.first_seq > seq_num;
            }),
        cfi_tracer_entries.end());
}

void
cfi_tracer::commit_until(ThreadID tid, InstSeqNum done_seq)
{
    cfi_tracer_entries.erase(
        std::remove_if(cfi_tracer_entries.begin(), cfi_tracer_entries.end(),
            [tid, done_seq](const cfi_tracer_entry_t& entry) {
                return entry.valid && entry.tid == tid &&
                    entry.last_seq <= done_seq;
            }),
        cfi_tracer_entries.end());
}

void
cfi_tracer::clear()
{
    cfi_tracer_entries.clear();
}
