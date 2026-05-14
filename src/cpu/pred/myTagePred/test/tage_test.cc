#include "tage.h"
#include "test_harness.h"

struct tage_test_access
{
  static tage_cmp &component(tage &t, std::size_t idx) {
    return t.component_list[idx];
  }

  static idx_t comp_idx(const tage &t, std::size_t idx) {
    return t.comp_idx[idx];
  }

  static std::uint64_t comp_tag(const tage &t, std::size_t idx) {
    return t.comp_tag[idx];
  }
};

static tage_cfg_t make_cfg(int comp_count, int depth) {
  tage_cfg_t cfg{};
  cfg.bimodal_depth = 16;
  cfg.ghistory_length = 0; // makes folded hash = 0
  cfg.comp_count = comp_count;
  cfg.pc_hash_start_for_idx = 2; // like bimodal
  cfg.pc_hash_width_for_idx = 4;
  cfg.pc_hash_start_for_tag = 0;
  cfg.pc_hash_width_for_tag = 4;
  cfg.allocate_randomplacement = false;
  cfg.random_type = SIMRAND;
  cfg.table_cfg.resize(comp_count);
  for (int i = 0; i < comp_count; ++i) {
    cfg.table_cfg[i].depth = depth;
    cfg.table_cfg[i].usefull_width = 2;
    cfg.table_cfg[i].ctr_width = 2;
    cfg.table_cfg[i].tag_width = 4;
    cfg.table_cfg[i].history_width = 0;
    cfg.table_cfg[i].pchistory_start = 0;
    cfg.table_cfg[i].pchistory_width = 0;
  }
  return cfg;
}

// Edge cases exercised:
// 1) No components -> bimodal fallback, counters saturate
// 2) Single component strengthens from not-taken to taken after updates
// 3) Different PC indices stay independent (no bleed between rows)
// 4) Weak provider path still returns true when bimodal is taken

static void test_bimodal_only_fallback() {
  auto cfg = make_cfg(0, 0);
  tage t(cfg);

  // Default bimodal counter is 2 -> predicts taken
  EXPECT_EQ(t.getPrediction(0x0), true);

  // Drive bimodal counter down: provider==-1 path updates bimodal
  t.update(0x0, false); // 2 -> 1
  EXPECT_EQ(t.getPrediction(0x0), false);

  // Another false should stay at 0
  t.update(0x0, false);
  EXPECT_EQ(t.getPrediction(0x0), false);
}

static void test_single_component_learns_taken() {
  auto cfg = make_cfg(1, 8);
  tage t(cfg);
  const addr_t a0 = 0x0;

  // Initial ctr=0 -> strong not-taken
  EXPECT_EQ(t.getPrediction(a0), false);

  // Train taken twice: 0->1 (weak) ->2 (weak hi)
  t.update(a0, true);
  t.update(a0, true);

  // Now weak taken but bimodal also taken, so predicts taken
  EXPECT_EQ(t.getPrediction(a0), true);
}

static void test_indices_are_independent() {
  auto cfg = make_cfg(1, 8);
  tage t(cfg);
  const addr_t a0 = 0x0; // idx 0
  const addr_t a1 = 0x4; // idx 1

  // Train a0 to taken (prediction before each update to set provider)
  t.getPrediction(a0);
  t.update(a0, true);
  t.getPrediction(a0);
  t.update(a0, true);

  // a0 should predict taken
  EXPECT_EQ(t.getPrediction(a0), true);
  // Drive bimodal for a1 downward so provider-less path returns not-taken
  t.getPrediction(a1);
  t.update(a1, false); // bimodal 2->1
  EXPECT_EQ(t.getPrediction(a1), false);
}

static void test_weak_provider_uses_bimodal_taken() {
  auto cfg = make_cfg(1, 8);
  tage t(cfg);
  const addr_t a0 = 0x0;

  t.getPrediction(a0); // init comp_row/provider state
  t.update(a0, true);  // 0->1 (weak)

  // With bimodal default 2 (taken), weak provider should fall back and return
  // taken.
  EXPECT_EQ(t.getPrediction(a0), true);
}

static void test_allocation_uses_final_prediction() {
  auto cfg = make_cfg(2, 8);
  tage t(cfg);
  const addr_t a0 = 0x4;

  t.getPrediction(a0);
  auto &provider_row =
      tage_test_access::component(t, 0).row(tage_test_access::comp_idx(t, 0));
  auto &upper_row =
      tage_test_access::component(t, 1).row(tage_test_access::comp_idx(t, 1));

  provider_row.tag = tage_test_access::comp_tag(t, 0);
  provider_row.ctr = 1; // weak not-taken
  provider_row.u = 0;

  upper_row.tag =
      static_cast<std::uint64_t>((tage_test_access::comp_tag(t, 1) + 1) & 0xF);
  upper_row.ctr = 0;
  upper_row.u = 0;

  EXPECT_EQ(t.getPrediction(a0), true); // weak provider falls back to bimodal

  t.update(a0, false);

  EXPECT_EQ(upper_row.tag, tage_test_access::comp_tag(t, 1));
  EXPECT_EQ(upper_row.ctr, static_cast<std::uint8_t>(1));
}

static void test_lfsr_allocation_target_is_bounded() {
  auto cfg = make_cfg(4, 8);
  cfg.allocate_randomplacement = true;
  cfg.random_type = LFSR;
  cfg.lfsr_width = 4;
  cfg.lfsr_seed = 1;

  tage t(cfg);
  const addr_t a0 = 0x4;

  t.getPrediction(a0);
  auto &low_row =
      tage_test_access::component(t, 1).row(tage_test_access::comp_idx(t, 1));
  auto &free_row_2 =
      tage_test_access::component(t, 2).row(tage_test_access::comp_idx(t, 2));
  auto &free_row_3 =
      tage_test_access::component(t, 3).row(tage_test_access::comp_idx(t, 3));

  for (int i = 0; i < cfg.comp_count; ++i) {
    auto &row =
        tage_test_access::component(t, static_cast<std::size_t>(i))
            .row(tage_test_access::comp_idx(t, static_cast<std::size_t>(i)));
    row.tag = static_cast<std::uint64_t>((tage_test_access::comp_tag(
                                              t, static_cast<std::size_t>(i)) +
                                          1) &
                                         0xF);
    row.ctr = 0;
    row.u = 0;
  }

  low_row.tag = tage_test_access::comp_tag(t, 1);
  low_row.ctr =
      1; // weak not-taken, so final prediction comes from bimodal=true

  free_row_2.tag =
      static_cast<std::uint64_t>((tage_test_access::comp_tag(t, 2) + 1) & 0xF);
  free_row_3.tag =
      static_cast<std::uint64_t>((tage_test_access::comp_tag(t, 3) + 1) & 0xF);

  EXPECT_EQ(t.getPrediction(a0), true);

  t.update(a0, false);

  EXPECT_EQ(free_row_2.tag, tage_test_access::comp_tag(t, 2));
  EXPECT_EQ(free_row_3.tag, static_cast<std::uint64_t>(
                                (tage_test_access::comp_tag(t, 3) + 1) & 0xF));
}

int main() {
  test_bimodal_only_fallback();
  test_single_component_learns_taken();
  test_indices_are_independent();
  test_weak_provider_uses_bimodal_taken();
  test_allocation_uses_final_prediction();
  test_lfsr_allocation_target_is_bounded();
  return test_harness::finish();
}
