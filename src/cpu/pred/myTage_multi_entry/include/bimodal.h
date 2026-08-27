#pragma once

#include <cstddef>
#include <vector>

#include "cfg.h"

typedef struct
{
    ctr2_t ctr;
} bimodal_entry_t;

class bimodal
{
  public:
    explicit bimodal(std::size_t bimodal_depth = 0,
                     std::size_t ctrs_per_row = 1, int offset_shift = 2,
                     std::size_t fetch_line_bytes = 0);
    ~bimodal() = default;

    ctr2_t getPrediction(addr_t addr) const;
    ctr2_t getPrediction(addr_t index_addr, addr_t lookup_addr) const;
    void update(addr_t addr, ctr2_t new_ctr);
    void update(addr_t index_addr, addr_t lookup_addr, ctr2_t new_ctr);

  private:
    std::size_t rowCount = 0;
    std::size_t ctrsPerRow = 1;
    int offsetShift = 2;
    std::size_t fetchLineBytes = 0;
    std::vector<bimodal_entry_t> bimodal_table;
    int idxWidth = 0;

    idx_t rowIndex(addr_t index_addr) const;
    std::size_t bankIndex(addr_t lookup_addr) const;
    std::size_t flatIndex(addr_t index_addr, addr_t lookup_addr) const;
};
