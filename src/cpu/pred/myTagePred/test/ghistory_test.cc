#include "cfg.h" // for idx_t typedef
#include "ghistory.h"
#include "test_harness.h"

// Edge cases covered:
// 1) history_length == 0 → no crash, hash == 0
// 2) reset clears all bits
// 3) push rotates ring buffer correctly
// 4) hash_foldedHistory with width 1 folds to parity-like xor

static void test_zero_length_no_crash() {
    ghistory gh(0);
    gh.push(true);
    EXPECT_EQ(gh.hash_foldedHistory(1, 1), 0);
}

static void test_reset_clears() {
    ghistory gh(4);
    gh.push(true);
    gh.push(true);
    gh.reset();
    EXPECT_EQ(gh.hash_foldedHistory(4, 1), 0);
}

static void test_push_rotation_and_hash_width1() {
    ghistory gh(4);
    gh.push(true);  // bits by age: 1
    gh.push(false); // 0,1
    gh.push(true);  // 1,0,1
    gh.push(true);  // 1,1,0,1
    // width=1 hashes chunk-by-chunk (xor of individual bits) -> 1^1^0^1 = 1
    EXPECT_EQ(gh.hash_foldedHistory(4, 1), 1);
}

static void test_push_rotation_and_hash_width2() {
    ghistory gh(4);
    gh.push(true);  // bits by age: 1
    gh.push(false); // 0,1
    gh.push(true);  // 1,0,1
    gh.push(true);  // 1,1,0,1
    gh.push(false);  // 1,0,1,0
    // width=1 hashes chunk-by-chunk (xor of individual bits) -> 1^0,0^1 = 11
    EXPECT_EQ(gh.hash_foldedHistory(4, 2), 3);
}

static void test_hash_uses_requested_prefix_of_history() {
    ghistory gh(8);
    gh.push(true);   // 1
    gh.push(false);  // 0,1
    gh.push(true);   // 1,0,1
    gh.push(true);   // 1,1,0,1
    gh.push(false);  // 0,1,1,0,1
    EXPECT_EQ(gh.hash_foldedHistory(3, 2), 3);
}

static void test_hash_can_start_from_older_history() {
    ghistory gh(8);
    gh.push(true);   // 1
    gh.push(false);  // 0,1
    gh.push(true);   // 1,0,1
    gh.push(true);   // 1,1,0,1
    gh.push(false);  // 0,1,1,0,1
    // start=1, len=3 -> ages [1,2,3] => 1,1,0 -> 0b11 ^ 0b00 = 3
    EXPECT_EQ(gh.hash_foldedHistory(1, 3, 2), 3);
}

int main() {
    test_zero_length_no_crash();
    test_reset_clears();
    test_push_rotation_and_hash_width1();
    test_push_rotation_and_hash_width2();
    test_hash_uses_requested_prefix_of_history();
    test_hash_can_start_from_older_history();
    return test_harness::finish();
}
