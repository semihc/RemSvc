#ifndef ABSEIL_STRINGS_HH
#define ABSEIL_STRINGS_HH
#pragma once

// Convenience header: pulls in the most-used Abseil string utilities and
// injects them into the RS namespace via `using namespace absl`.
// Include this instead of individual absl/strings/* headers when you need
// StrCat, StrJoin, StrFormat, etc. within RS code.
// Abseil includes
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_replace.h>
#include <absl/strings/str_split.h>
#include <absl/strings/substitute.h>

#include <absl/strings/str_format.h>

namespace RS {

  // Assist for lookups
  using namespace absl;

  // Shortcuts
  // using alias = original type ;


} // end namespace

#endif // Include guard
