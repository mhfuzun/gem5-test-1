#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

class plru
{
  public:
    explicit plru(std::size_t way_count = 1)
    {
        reset(way_count);
    }

    void reset(std::size_t way_count)
    {
        ways = std::max<std::size_t>(1, way_count);
        leaves = 1;
        while (leaves < ways) {
            leaves <<= 1;
        }
        bit_count = leaves > 1 ? leaves - 1 : 0;
    }

    std::vector<std::uint8_t> new_state() const
    {
        return std::vector<std::uint8_t>(bit_count, 0);
    }

    void ensure_state(std::vector<std::uint8_t>& state) const
    {
        if (state.size() != bit_count) {
            state.assign(bit_count, 0);
        }
    }

    void touch(std::vector<std::uint8_t>& state, std::size_t way) const
    {
        ensure_state(state);
        if (bit_count == 0 || way >= ways) {
            return;
        }

        std::size_t node = 0;
        std::size_t low = 0;
        std::size_t high = leaves;
        while (node < bit_count && high - low > 1) {
            const std::size_t mid = low + (high - low) / 2;
            const bool went_right = way >= mid;
            // Bit value points to the subtree that should be considered LRU.
            state[node] = went_right ? 0 : 1;
            if (went_right) {
                node = node * 2 + 2;
                low = mid;
            } else {
                node = node * 2 + 1;
                high = mid;
            }
        }
    }

    std::size_t get_lru(std::vector<std::uint8_t>& state) const
    {
        ensure_state(state);
        if (bit_count == 0) {
            return 0;
        }

        std::size_t node = 0;
        std::size_t low = 0;
        std::size_t high = leaves;
        while (node < bit_count && high - low > 1) {
            const bool go_right = state[node] != 0;
            const std::size_t mid = low + (high - low) / 2;
            if (go_right) {
                node = node * 2 + 2;
                low = mid;
            } else {
                node = node * 2 + 1;
                high = mid;
            }
        }

        return low < ways ? low : ways - 1;
    }

    std::size_t get_lru_and_touch(std::vector<std::uint8_t>& state) const
    {
        const std::size_t way = get_lru(state);
        touch(state, way);
        return way;
    }

  private:
    std::size_t ways = 1;
    std::size_t leaves = 1;
    std::size_t bit_count = 0;
};
