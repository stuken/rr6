// rr6 - ReXGlue Recompiled Project
//
// RR6_* logging macros bound to the rr6 log category. Ported from reblue's
// core/logging.h (Copyright (c) 2026 Tom Clay, BSD 3-Clause).

#pragma once

#include <atomic>

#include <rex/logging.h>

#include "core/settings.h"

REXLOG_DEFINE_CATEGORY(rr6)

#define RR6_TRACE(...) REXLOG_CAT_TRACE(::rex::log::rr6(), __VA_ARGS__)
#define RR6_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::rr6(), __VA_ARGS__)
#define RR6_INFO(...) REXLOG_CAT_INFO(::rex::log::rr6(), __VA_ARGS__)
#define RR6_WARN(...) REXLOG_CAT_WARN(::rex::log::rr6(), __VA_ARGS__)
#define RR6_ERROR(...) REXLOG_CAT_ERROR(::rex::log::rr6(), __VA_ARGS__)
#define RR6_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::rr6(), __VA_ARGS__)

#define RR6_DEV_WARN(...)                                                      \
  do {                                                                         \
    if (::rr6::Settings::Get().Devmode())                                      \
      RR6_WARN(__VA_ARGS__);                                                   \
  } while (0)

#define RR6_DEV_INFO(...)                                                      \
  do {                                                                         \
    if (::rr6::Settings::Get().Devmode())                                      \
      RR6_INFO(__VA_ARGS__);                                                   \
  } while (0)

// Warns on the first `limit` hits of one call site. Marks paths the renderer
// doesn't model yet, so a run shows which ones the game reaches.
#define RR6_WARN_FIRST(limit, ...)                                             \
  do {                                                                         \
    static std::atomic<unsigned> s_rr6_warned{0};                              \
    if (s_rr6_warned.fetch_add(1, std::memory_order_relaxed) < (limit))        \
      RR6_WARN(__VA_ARGS__);                                                   \
  } while (0)
