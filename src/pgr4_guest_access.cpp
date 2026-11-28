// The recompiled code's guest memory accesses, as its own precompiled header defines them (local
// change, 26/11/2026, branch cpu_thread). This file is built in the pgr4_recomp target, with the
// generated pgr4_pch.h and the same definitions as the generated sources, so the log line says what
// the game's code really uses: relaxed atomics (the SDK's pch_h.inja, REX_GUEST_LOAD...), or the
// volatile accesses of before (the CMake option PGR4_VOLATILE_GUEST_ACCESS).
#include <rex/logging.h>

void Pgr4LogGuestAccess() {
#if !defined(REX_GUEST_LOAD)
  REXLOG_INFO("PGR4 recompiled code: guest memory accesses volatile (generated before REX_GUEST_LOAD)");
#elif defined(REX_VOLATILE_GUEST_ACCESS)
  REXLOG_INFO("PGR4 recompiled code: guest memory accesses volatile, as before "
              "(PGR4_VOLATILE_GUEST_ACCESS)");
#else
  REXLOG_INFO("PGR4 recompiled code: guest memory accesses relaxed atomics, vectors volatile");
#endif
}
