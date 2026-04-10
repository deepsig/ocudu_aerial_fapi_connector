// SPDX-FileCopyrightText: Copyright (c) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Workaround: patch fmt v11's consteval detection for GCC 12.3.
// fmt/base.h unconditionally defines FMT_USE_CONSTEVAL=1 when __cpp_consteval
// is available, but GCC 12.3 has bugs with consteval in certain template
// instantiation contexts. We include fmt/base.h, then undo the consteval.
#pragma once
#include "fmt/base.h"
#ifdef FMT_USE_CONSTEVAL
#  undef FMT_USE_CONSTEVAL
#  define FMT_USE_CONSTEVAL 0
#endif
#ifdef FMT_CONSTEVAL
#  undef FMT_CONSTEVAL
#  define FMT_CONSTEVAL constexpr
#endif
