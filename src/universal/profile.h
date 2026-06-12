#pragma once

#ifdef TRACY_ENABLE
#ifndef TRACY_ON_DEMAND
#error This should be left ON
#endif

#include <tracy/public/tracy/Tracy.hpp>
#include <tracy/public/tracy/TracyC.h>
//#include <tracy/Tracy.hpp>
//#include <tracy/TracyC.h>

#define PROF_SCOPED_RUNTIME_NAME(name) ZoneScoped; ZoneName(name, strlen(name));
#define PROF_SCOPED(name) ZoneScopedN(name)
#define PROFLOAD_SCOPED(name) PROF_SCOPED(name)
#define PROF_THREADNAME(threadname) tracy::SetThreadName(threadname)
#else

#if defined(__EMSCRIPTEN__) && defined(__cplusplus)
// Auto-profiler: reuse the engine's existing PROF_SCOPED zones as a runtime-gated, per-thread
// SELF-TIME profiler (enable with ?kbprof). Near-zero cost when off (one inline branch per zone).
// Each engine thread accumulates self-time per zone name and periodically dumps its top zones to
// the console, tagged by thread id — so we find the backend "other" hotspot without hand-timers.
// Implementation lives in gl_query.cpp (keeps emscripten.h/STL out of this widely-included header).
namespace kbprof {
    extern int g_on;                           // -1 unread, 0 off, 1 on
    int    Init();                             // resolve ?kbprof on first use
    double Now();                              // emscripten_get_now wrapper
    void   Enter();                            // push a child-time frame
    void   Exit(const char* name, double t0);  // pop, compute self-time, record, maybe dump
    inline int On() { return g_on >= 0 ? g_on : Init(); }
    struct Zone {
        const char* n_; double t0_;
        explicit Zone(const char* n) : n_(n), t0_(0.0) { if (On()) { t0_ = Now(); Enter(); } }
        ~Zone() { if (t0_ != 0.0) Exit(n_, t0_); }
        Zone(const Zone&) = delete; Zone& operator=(const Zone&) = delete;
    };
}
#define KBPROF_CAT2(a,b) a##b
#define KBPROF_CAT1(a,b) KBPROF_CAT2(a,b)
// Trailing ';' is INSIDE the macro: many engine sites write PROF_SCOPED("x") with no semicolon
// (harmless when it was a no-op). With the ';' here both PROF_SCOPED("x") and PROF_SCOPED("x");
// compile (the latter just adds an empty statement; -w suppresses the warning).
#define PROF_SCOPED(name) ::kbprof::Zone KBPROF_CAT1(kbz_,__LINE__)(name);
#else
#define PROF_SCOPED(name) // Disable Profiling without Tracy
#endif
#define PROF_SCOPED_RUNTIME_NAME(name)
#define PROFLOAD_SCOPED(name) PROF_SCOPED(name)
#define ZoneText(str, len)
#define ZoneTextF(fmt, ...)
#define ZoneName(str, len)
#define PROF_THREADNAME(threadname)
#define FrameMark

#endif

