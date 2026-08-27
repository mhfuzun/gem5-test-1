#pragma once

#include <cassert>
#include <cstdint>

class lfsr
{
  public:
    lfsr(int width, std::uint64_t seed) : width(width), initialValue(seed)
    {
        assert(width > 0 && width <= 64);
        reset();
    }

    void reset()
    {
        lfsr_value = initialValue;
        lfsr_width_mask();
        if (lfsr_value == 0)
            lfsr_value = 1;
    }

    std::uint64_t nextRand()
    {
        std::uint64_t bit = ((lfsr_value >> 0) ^ (lfsr_value >> 2) ^
                             (lfsr_value >> 3) ^ (lfsr_value >> 5)) &
                            1;

        lfsr_value = (lfsr_value >> 1) | (bit << (width - 1));
        lfsr_width_mask();
        return lfsr_value;
    }

    std::uint64_t foldedhash(int width)
    {
        assert(width > 0 && width <= 64);

        std::uint64_t x = lfsr_value;

        // width'e göre dinamik folding
        int shift = width;
        while (shift < 64) {
            x ^= lfsr_value >> shift;
            shift <<= 1;
        }

        if (width < 64)
            x &= (1ULL << width) - 1;

        return x;
    }

    void updateRand(std::uint64_t val)
    {
        lfsr_value ^= val;
        lfsr_width_mask();
        if (lfsr_value == 0)
            lfsr_value = 1;
    }

  private:
    int width;
    std::uint64_t initialValue;
    std::uint64_t lfsr_value;

    void lfsr_width_mask()
    {
        if (width < 64)
            lfsr_value &= ((1ULL << width) - 1);
    }
};
