#pragma once

#include <cstdint>

class ghistory_register
{
  public:
    ghistory_register(int width, int length);

    void reset();
    std::uint64_t getHash() const;
    void updateHash(bool new_bit, bool old_bit);

  private:
    int width;
    int length;
    int insert_point;
    std::uint64_t register_value = 0;
    std::uint64_t mask = 0;
};
