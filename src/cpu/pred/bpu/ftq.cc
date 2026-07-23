#include <algorithm>
#include <cstddef>

#include "ftq.hh"

ftq::ftq(ftq_cfg cfg)
{
    this->cfg = cfg;
}

ftq_entry_t*
ftq::add_entry(int base_addr, int fetch_span_2b)
{
    if (full()) {
        return nullptr;
    }

    ftq_entry_t entry;
    entry.valid = true;
    entry.base_addr = base_addr;
    entry.fetch_span_2b = fetch_span_2b;

    ftq_entries.push_back(entry);
    return &ftq_entries.back();
}

void
ftq::add_fetch_span(ftq_entry_t& entry, int fetch_span_2b)
{
    entry.fetch_span_2b += fetch_span_2b;
}

void
ftq::add_bpu2pushes(ftq_entry_t& entry,
                    std::vector<bpu_sign_t> cfi_sign_vector,
                    bpu_sign_t cfi_taken_sign)
{
    entry.cfi_sign_vector = cfi_sign_vector;
    entry.cfi_taken_sign = cfi_taken_sign;
}

void
ftq::add_bpu3pushes(ftq_entry_t& entry, int target)
{
    entry.target = target;
}

void
ftq::set_jalr_fail(ftq_entry_t& entry, bool jalr_fail)
{
    entry.jalr_fail = jalr_fail;
}

int
ftq::get_fetch_span_2b() const
{
    const ftq_entry_t* entry = front();
    return entry == nullptr ? 0 : entry->fetch_span_2b;
}

void
ftq::consume(int two_byte_count)
{
    // kullanılan 16 bit sayısı queue front entry'den düşülür.
    // eğer span değeri biterse sadece ikinci bir entry varsa yapılır.
    // ikinci bir entry yoksa beklenir.
    // FTQ'nin front entry'si IFU/FQ tarafının şu an tükettiği fetch block
    // yol haritasıdır. Span tamamen bitince ve arkada daha genç bir entry
    // varsa front ilerletilebilir; arkada entry yokken pop yapılmaz, çünkü
    // frontend'in devam edecek doğrulanmış/predicted bloğu henüz yoktur. Gem5
    // entegrasyonunda bu durum fetch tarafında FTQ-empty stall olarak
    // sayılabilir.
    if (ftq_entries.empty() || two_byte_count <= 0) {
        return;
    }

    ftq_entry_t& entry = ftq_entries.front();
    entry.fetch_span_2b = std::max(0, entry.fetch_span_2b - two_byte_count);

    if (entry.fetch_span_2b == 0 && ftq_entries.size() > 1) {
        ftq_entries.pop_front();
    }
}

ftq_entry_t*
ftq::front()
{
    if (ftq_entries.empty()) {
        return nullptr;
    }

    return &ftq_entries.front();
}

const ftq_entry_t*
ftq::front() const
{
    if (ftq_entries.empty()) {
        return nullptr;
    }

    return &ftq_entries.front();
}

ftq_entry_t*
ftq::back()
{
    if (ftq_entries.empty()) {
        return nullptr;
    }

    return &ftq_entries.back();
}

const ftq_entry_t*
ftq::back() const
{
    if (ftq_entries.empty()) {
        return nullptr;
    }

    return &ftq_entries.back();
}

bool
ftq::empty() const
{
    return ftq_entries.empty();
}

bool
ftq::full() const
{
    return cfg.depth > 0 &&
        ftq_entries.size() >= static_cast<std::size_t>(cfg.depth);
}

int
ftq::size() const
{
    return static_cast<int>(ftq_entries.size());
}

void
ftq::clear()
{
    ftq_entries.clear();
}
