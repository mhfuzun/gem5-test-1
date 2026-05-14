#pragma once

#include <cstdlib>
#include <iostream>

namespace test_harness {
inline int &failure_count() {
  static int n = 0;
  return n;
}

inline void fail(const char *file, int line, const char *msg) {
  ++failure_count();
  std::cerr << file << ":" << line << " " << msg << "\n";
}

template <typename A, typename B>
inline void expect_eq_impl(const A &a, const B &b, const char *expr_a,
                           const char *expr_b, const char *file, int line) {
  if (!(a == b)) {
    std::cerr << file << ":" << line << " EXPECT_EQ failed: " << expr_a
              << " vs " << expr_b << " (lhs=" << a << ", rhs=" << b << ")\n";
    ++failure_count();
  }
}

#ifndef TESTNAME
#define TESTNAME "<file>"
#endif

inline int finish() {
  std::cout << "Test finished with: " << failure_count()
            << " error(s), file: " << TESTNAME << std::endl;
  return failure_count() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
} // namespace test_harness

#define EXPECT_TRUE(x)                                                        \
  do {                                                                        \
    if (!(x))                                                                 \
      ::test_harness::fail(__FILE__, __LINE__, "EXPECT_TRUE failed: " #x);    \
  } while (0)

#define EXPECT_EQ(a, b)                                                       \
  do {                                                                        \
    ::test_harness::expect_eq_impl((a), (b), #a, #b, __FILE__, __LINE__);     \
  } while (0)
