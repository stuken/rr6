// rr6 - ReXGlue Recompiled Project

#include "platform/guest_abort_log.h"

#include <csignal>
#include <format>
#include <string>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "core/logging.h"

namespace rr6::platform {

namespace {

void OnAbort(int sig) {
  if (auto *ts = rex::runtime::ThreadState::Get()) {
    const auto *c = ts->context();
    RR6_CRITICAL("guest context at abort: lr={:08X} ctr={:08X} r1={:08X} "
                 "r3={:08X} r4={:08X} r5={:08X} r30={:08X} r31={:08X}",
                 u32(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32,
                 c->r5.u32, c->r30.u32, c->r31.u32);
  } else {
    RR6_CRITICAL("abort on a thread with no guest context");
  }
  rex::FlushLogging();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

} // namespace

#if defined(_WIN32)
extern "C" char __ImageBase;
#endif

// Runs after the SDK's handlers, so it only sees faults nothing recovered.
// The host pc is logged relative to the exe so it resolves offline with
// llvm-symbolizer --obj=rr6.exe 0x<offset>; it names the recompiled guest
// function that faulted.
bool OnException(rex::arch::Exception *ex, void *) {
  u64 image_base = 0;
#if defined(_WIN32)
  image_base = reinterpret_cast<u64>(&__ImageBase);
#endif
  const u64 pc = ex->pc();
  RR6_CRITICAL("host exception code={} pc=rr6.exe+0x{:X} (0x{:016X}) fault=0x{:016X}",
               static_cast<int>(ex->code()), pc - image_base, pc,
               ex->code() == rex::arch::Exception::Code::kAccessViolation
                   ? ex->fault_address()
                   : 0);
#if defined(_WIN32)
  // Return addresses on the faulting host stack that fall inside the exe,
  // innermost first: a heuristic backtrace for llvm-symbolizer.
  if (const auto *hc = ex->thread_context()) {
    const auto *nt = reinterpret_cast<const u8 *>(image_base) +
                     *reinterpret_cast<const u32 *>(image_base + 0x3C);
    const u32 image_size = *reinterpret_cast<const u32 *>(nt + 0x50);
    const auto *sp = reinterpret_cast<const u64 *>(hc->rsp);
    std::string trace;
    u32 found = 0;
    for (u32 i = 0; i < 1024 && found < 16; ++i) {
      const u64 v = sp[i];
      if (v > image_base && v < image_base + image_size) {
        trace += std::format(" 0x{:X}", v - image_base);
        ++found;
      }
    }
    RR6_CRITICAL("host stack (rr6.exe offsets):{}", trace);
  }
#endif
  if (auto *ts = rex::runtime::ThreadState::Get()) {
    const auto *c = ts->context();
    RR6_CRITICAL("guest context: lr={:08X} ctr={:08X} r1={:08X} r3={:08X} "
                 "r4={:08X} r5={:08X} r30={:08X} r31={:08X}",
                 u32(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32,
                 c->r5.u32, c->r30.u32, c->r31.u32);
  }
  rex::FlushLogging();
  return false;
}

void InstallGuestAbortLogger() {
  std::signal(SIGABRT, OnAbort);
  rex::arch::ExceptionHandler::Install(OnException, nullptr);
}

} // namespace rr6::platform
