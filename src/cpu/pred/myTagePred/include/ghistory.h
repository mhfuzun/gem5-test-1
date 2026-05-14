#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class ghistory
{
public:
  explicit ghistory(int history_length = 0);
  ghistory(const ghistory &other) = default;
  ~ghistory() = default;

  void reset();
  int hash_foldedHistory(std::size_t start, std::size_t len, int width) const;
  int hash_foldedHistory(std::size_t len, int width) const;
  void push(bool value);

private:
  std::vector<std::uint8_t> bits; // ring buffer of 0/1
  std::size_t head = 0;           // index of most-recent bit
};
