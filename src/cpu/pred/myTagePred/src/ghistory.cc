#include "cpu/pred/myTagePred/include/ghistory.h"

ghistory::ghistory(int history_length)
    : bits(history_length > 0 ? static_cast<std::size_t>(history_length) : 0,
           0),
      head(0) {}

void ghistory::reset() {
  for (auto &b : bits)
    b = 0;
  head = 0;
}

int ghistory::hash_foldedHistory(std::size_t start, std::size_t len,
                                 int width) const {
  if (len == 0 || width <= 0 || bits.empty() || start >= bits.size())
    return 0;

  const std::size_t available = bits.size() - start;
  const std::size_t used_len = (len < available) ? len : available;
  int hash = 0;

  for (std::size_t i = 0; i < used_len; i += static_cast<std::size_t>(width)) {
    int chunk = 0;
    for (int k = 0; k < width; ++k) {
      const std::size_t age = i + static_cast<std::size_t>(k);
      if (age >= used_len)
        break;
      const std::size_t idx =
          (head + start + age) % bits.size(); // age=0 -> most recent
      chunk |= (static_cast<int>(bits[idx]) & 1) << k;
    }
    hash ^= chunk;
  }

  return hash;
}

int ghistory::hash_foldedHistory(std::size_t len, int width) const {
  return hash_foldedHistory(0, len, width);
}

void ghistory::push(bool value) {
  if (bits.empty())
    return;

  // Move head "back" so age=0 is the new value.
  head = (head + bits.size() - 1) % bits.size();
  bits[head] = value ? 1 : 0;
}
