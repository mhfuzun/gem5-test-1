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
        explicit bimodal(std::size_t bimodal_depth = 0);
        ~bimodal() = default;

        ctr2_t getPrediction(addr_t addr) const;
        void update(addr_t addr, ctr2_t new_ctr);

    private:
        std::vector<bimodal_entry_t> bimodal_table;
        int idxWidth = 0;
};
