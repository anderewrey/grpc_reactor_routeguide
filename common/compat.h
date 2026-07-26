///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#ifndef COMMON_COMPAT_H_
#define COMMON_COMPAT_H_

#include <type_traits>

// C++23 backport for std::to_underlying
// Only define if not already available (C++23+)
#ifndef __cpp_lib_to_underlying
namespace std {
template <typename Enum>
// A standard-sanctioned backport, gated on the feature-test macro above so it vanishes once the
// toolchain provides it.
// NOLINTNEXTLINE(bugprone-std-namespace-modification)
constexpr underlying_type_t<Enum> to_underlying(Enum e) noexcept {
  return static_cast<underlying_type_t<Enum>>(e);
}
}  // namespace std
#endif  // __cpp_lib_to_underlying

#endif  // COMMON_COMPAT_H_
