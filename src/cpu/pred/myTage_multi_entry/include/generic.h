#pragma once

#include <cstdint>
#include <limits>

class SaturatingCounter
{
  public:
    template<typename T>
    static inline void inc(T& value, T max_val)
    {
        if (value < max_val)
            ++value;
    }

    template<typename T>
    static inline void dec(T& value, T min_val = 0)
    {
        if (value > min_val)
            --value;
    }

    template<typename T>
    static inline void update_bits(T& value, bool taken, int bitWidth)
    {
        if (bitWidth <= 0)
            return;

        const std::uint64_t max_val =
            (bitWidth >= 64)
                ? std::numeric_limits<std::uint64_t>::max()
                : ((std::uint64_t{1} << static_cast<std::uint64_t>(bitWidth)) -
                   1);

        std::uint64_t cur = static_cast<std::uint64_t>(value);
        if (taken) {
            if (cur < max_val)
                ++cur;
        } else {
            if (cur > 0)
                --cur;
        }

        value = static_cast<T>(cur);
    }
};
