#include <algorithm>
#include <cstddef>

#include "bimodal.h"

bimodal::bimodal(std::size_t bimodal_depth, std::size_t ctrs_per_row,
                 int offset_shift, std::size_t fetch_line_bytes)
    : rowCount(bimodal_depth),
      ctrsPerRow(std::max<std::size_t>(ctrs_per_row, 1)),
      offsetShift(std::max(offset_shift, 0)), fetchLineBytes(fetch_line_bytes),
      bimodal_table(rowCount * ctrsPerRow)
{
    idxWidth = idxWidthForDepth(bimodal_depth);
    // Weakly taken by default (2-bit counter: 2).
    for (auto& e : bimodal_table)
        e.ctr = 2;
}

ctr2_t
bimodal::getPrediction(addr_t addr) const
{
    return getPrediction(addr, addr);
}

ctr2_t
bimodal::getPrediction(addr_t index_addr, addr_t lookup_addr) const
{
    if (bimodal_table.empty())
        return 2;

    return bimodal_table[flatIndex(index_addr, lookup_addr)].ctr;
}

void
bimodal::update(addr_t addr, ctr2_t new_ctr)
{
    update(addr, addr, new_ctr);
}

void
bimodal::update(addr_t index_addr, addr_t lookup_addr, ctr2_t new_ctr)
{
    if (bimodal_table.empty())
        return;

    bimodal_table[flatIndex(index_addr, lookup_addr)].ctr = new_ctr;
}

idx_t
bimodal::rowIndex(addr_t index_addr) const
{
    if (rowCount == 0)
        return 0;

    idx_t idx = getIdx(index_addr, 2, idxWidth);
    idx %= static_cast<idx_t>(rowCount);
    return idx;
}

std::size_t
bimodal::bankIndex(addr_t lookup_addr) const
{
    if (ctrsPerRow == 0)
        return 0;

    const addr_t offset =
        fetchLineBytes == 0
            ? lookup_addr
            : (lookup_addr % static_cast<addr_t>(fetchLineBytes));
    const addr_t shifted =
        offsetShift == 0
            ? offset
            : (offsetShift >= 64
                   ? 0
                   : (offset >> static_cast<addr_t>(offsetShift)));
    return static_cast<std::size_t>(shifted % static_cast<addr_t>(ctrsPerRow));
}

std::size_t
bimodal::flatIndex(addr_t index_addr, addr_t lookup_addr) const
{
    const auto row = static_cast<std::size_t>(rowIndex(index_addr));
    const auto bank = bankIndex(lookup_addr);
    return row * ctrsPerRow + bank;
}
