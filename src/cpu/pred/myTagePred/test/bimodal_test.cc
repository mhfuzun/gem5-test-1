#include "bimodal.h"
#include "test_harness.h"

static void test_defaults() {
  const std::size_t bim_depth = 1024;
  bimodal bim(bim_depth);

  // Default is weakly-taken (ctr=2).
  EXPECT_EQ(bim.getPrediction(0x0), static_cast<ctr2_t>(2));
  EXPECT_EQ(bim.getPrediction(0x4), static_cast<ctr2_t>(2));
}

static void test_update_affects_index() {
  const std::size_t bim_depth = 1024;
  bimodal bim(bim_depth);

  const addr_t a0 = 0x0; // idx = (0x0 >> 2) & 1023 = 0
  const addr_t a1 = 0x4; // idx = 1

  bim.update(a0, static_cast<ctr2_t>(3));
  EXPECT_EQ(bim.getPrediction(a0), static_cast<ctr2_t>(3));
  EXPECT_EQ(bim.getPrediction(a1), static_cast<ctr2_t>(2));
}

static void test_index_wrap_same_entry() {
  const std::size_t bim_depth = 1024;
  bimodal bim(bim_depth);

  const addr_t a0 = 0x0;                  // idx 0
  const addr_t a0_alias =
      (1024ULL << 2); // (addr>>2) = 1024 -> idx 0 after mod

  bim.update(a0, static_cast<ctr2_t>(1));
  EXPECT_EQ(bim.getPrediction(a0_alias), static_cast<ctr2_t>(1));
}

int main() {
  test_defaults();
  test_update_affects_index();
  test_index_wrap_same_entry();
  return test_harness::finish();
}
