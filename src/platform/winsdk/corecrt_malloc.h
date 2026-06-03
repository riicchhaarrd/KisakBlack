// corecrt_malloc.h — portable stand-in for MSVC's CRT malloc header.
// Declare the allocators directly (NOT via <cstdlib>) to avoid pulling POSIX
// random(), which collides with the engine's own random().
#ifndef KISAK_CORECRT_MALLOC_H
#define KISAK_CORECRT_MALLOC_H
// Emscripten/musl declares the standard allocators in its own headers WITHOUT a
// noexcept spec; redeclaring them here with noexcept is an exception-spec mismatch
// under Clang. Drop the spec on the web build (musl prototypes win). The MSVC
// _aligned_* helpers are not in musl, so keep declaring those ourselves.
#ifdef __EMSCRIPTEN__
extern "C" {
    void *malloc(size_t);
    void *calloc(size_t, size_t);
    void *realloc(void *, size_t);
    void  free(void *);
    void *_aligned_malloc(size_t, size_t);
    void  _aligned_free(void *);
}
#else
extern "C" {
    void *malloc(size_t) noexcept;
    void *calloc(size_t, size_t) noexcept;
    void *realloc(void *, size_t) noexcept;
    void  free(void *) noexcept;
    void *_aligned_malloc(size_t, size_t) noexcept;
    void  _aligned_free(void *) noexcept;
}
#endif
#endif
