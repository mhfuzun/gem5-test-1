#include <stdexcept>

#include "cfg.h"
#include "ghistory_register.h"

ghistory_register::ghistory_register(int width_, int length_)
    : width(width_), length(length_), insert_point(0),
      mask(maskForWidth(width_))
{
    if (width <= 0 || width > 64)
        throw std::invalid_argument(
            "ghistory_register.width must be in [1, 64]");
    if (length <= 0)
        throw std::invalid_argument("ghistory_register.length must be > 0");

    insert_point = length % width;
}

void
ghistory_register::reset()
{
    register_value = 0;
}

std::uint64_t
ghistory_register::getHash() const
{
    return register_value & mask;
}

void
ghistory_register::updateHash(bool new_bit, bool old_bit)
{
    const std::uint64_t out_bit = (width == 64)
                                      ? (register_value >> 63)
                                      : ((register_value >> (width - 1)) & 1u);

    register_value = (register_value << 1) & mask;
    register_value ^= static_cast<std::uint64_t>(new_bit);
    register_value ^= out_bit;
    register_value ^= static_cast<std::uint64_t>(old_bit)
                      << static_cast<std::uint64_t>(insert_point);
    register_value &= mask;
}
