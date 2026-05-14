#include <cstddef>

#include "cpu/pred/myTagePred/include/bimodal.h"

bimodal::bimodal(std::size_t bimodal_depth)
    : bimodal_table(bimodal_depth)
{
    idxWidth = idxWidthForDepth(bimodal_depth);
    // Weakly taken by default (2-bit counter: 2).
    for (auto& e : bimodal_table)
        e.ctr = 2;
}

ctr2_t bimodal::getPrediction(addr_t addr) const {
    if (bimodal_table.empty())
        return 2;

    idx_t idx = getIdx(addr, 2, idxWidth);
    idx %= static_cast<idx_t>(bimodal_table.size());
    return bimodal_table[static_cast<std::size_t>(idx)].ctr;
}

void bimodal::update(addr_t addr, ctr2_t new_ctr) {
    if (bimodal_table.empty())
        return;

    idx_t idx = getIdx(addr, 2, idxWidth);
    idx %= static_cast<idx_t>(bimodal_table.size());
    bimodal_table[static_cast<std::size_t>(idx)].ctr = new_ctr;
}
