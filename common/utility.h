///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#pragma once

#include <type_traits>

// C++23 backport for std::to_underlying
// Only define if not already available (C++23+)
#ifndef __cpp_lib_to_underlying
namespace std {
template <typename Enum>
constexpr underlying_type_t<Enum> to_underlying(Enum e) noexcept {
  return static_cast<underlying_type_t<Enum>>(e);
}
}  // namespace std
#endif  // __cpp_lib_to_underlying
