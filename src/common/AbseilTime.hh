#ifndef ABSEIL_TIME_HH
#define ABSEIL_TIME_HH
#pragma once

// Convenience header: pulls in Abseil time utilities (Time, Duration, civil
// time arithmetic, Now()) and injects them into the RS namespace via
// `using namespace absl`.  Include this instead of individual absl/time/*
// headers when you need Now(), FormatTime(), or civil_second arithmetic.
// Abseil includes
#include <absl/time/time.h>
#include <absl/time/civil_time.h>
#include <absl/time/clock.h>

namespace RS {

  // Assist for lookups
  using namespace absl;

  // Shortcuts
  // using alias = original type ;


} // end namespace

#endif // Include guard
