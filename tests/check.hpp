/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
inline int& checks() {
  static int n = 0;
  return n;
}

struct Register {
  Register(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

inline void expect(bool ok, const char* expr, const char* file, int line) {
  ++checks();
  if (!ok) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, expr);
  }
}

inline int runAll() {
  for (const auto& c : cases()) {
    const int before = failures();
    c.fn();
    std::printf("%s %s\n", failures() == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("%d checks, %d failed\n", checks(), failures());
  return failures() == 0 && checks() > 0 ? 0 : 1;
}

}  // namespace check

#define TA_CAT2(a, b) a##b
#define TA_CAT(a, b) TA_CAT2(a, b)
#define TEST(name)                                                              \
  static void TA_CAT(test_, __LINE__)();                                        \
  static check::Register TA_CAT(reg_, __LINE__)(name, TA_CAT(test_, __LINE__)); \
  static void TA_CAT(test_, __LINE__)()
#define CHECK(expr) check::expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
