#include "cpu/o3/cfi_tracer.hh"

#include <algorithm>

cfi_tracer::cfi_tracer(int depth)
    : depth(std::max(1, depth))
{
}

void
cfi_tracer::add_fetch_block(ThreadID tid, int fetch_block_addr,
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
cfi_tracer::check(ThreadID tid, int fetch_block_addr,
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
            actual.compressed != predicted.compressed) {
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
cfi_tracer::lookup(ThreadID tid, int fetch_block_addr) const
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
cfi_tracer::make_btb_entry(ThreadID tid, int fetch_block_addr) const
{
    btb_entry_t btb_entry;
    const cfi_tracer_entry_t* entry = lookup(tid, fetch_block_addr);
    if (entry == nullptr || entry->cfi_points.empty()) {
        return btb_entry;
    }

    btb_entry.valid = true;

    auto fill_record = [](btb_entry_record_t& record,
                          const cfi_point_t& cfi_point) {
        record.valid = true;
        record.target = cfi_point.target;
        record.cfi_sign = cfi_point.sign;
        record.taken_next_cfi_span_2b = cfi_point.next_cfi_span_2b;
        record.fallthrough_next_cfi_span_2b = cfi_point.next_cfi_span_2b;
    };

    fill_record(btb_entry.e1, entry->cfi_points[0]);
    if (entry->cfi_points.size() > 1) {
        fill_record(btb_entry.e2, entry->cfi_points[1]);
    }

    return btb_entry;
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
