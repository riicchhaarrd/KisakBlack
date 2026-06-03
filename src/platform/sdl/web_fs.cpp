//============================================================================
// web_fs.cpp  (Emscripten only)
//
// Synchronous-looking file bridge over the Chrome File System Access API.
//
// The engine reads game data synchronously (FS_FileRead/fread and the
// db_file_load ReadFileEx path). The File System Access API (showDirectoryPicker
// -> FileSystemFileHandle.getFile() -> Blob.arrayBuffer()) is ASYNC, and the
// File/handle objects live on the browser MAIN (DOM) thread. There are TWO web
// builds, selected by __EMSCRIPTEN_PTHREADS__:
//
//   * Single-thread / fiber build (NO -pthread): bridge with **Asyncify**. Each
//     call uses EM_ASYNC_JS, which suspends the wasm stack, performs the async JS
//     read on the (only) thread, and resumes — so the C side sees a blocking call.
//
//   * Pthreads build (-pthread, this file's __EMSCRIPTEN_PTHREADS__ path): the
//     engine runs on Web Workers and the heap is a SharedArrayBuffer, but the
//     File objects still live on the DOM thread. We bridge with a **main-thread
//     proxy**: the calling worker fills a request, posts it to the DOM thread with
//     MAIN_THREAD_ASYNC_EM_ASM (fire-and-forget), then BLOCKS on an atomic doorbell
//     via emscripten_atomic_wait_u32. The DOM thread runs the async read against
//     the same path-index + 256 KB block cache (web_fs.js, KBFS), copies the bytes
//     straight into the shared HEAP at the caller's dst pointer (visible to the
//     worker because the heap is shared), stores the result, and Atomics.notify's
//     the doorbell to wake the worker. No Asyncify is needed.
//
// Files are streamed on demand (never fully resident): a read materializes exactly
// the requested [offset,len) slice from the underlying Blob via Blob.slice, so a
// multi-GB .ff is never brought into the wasm heap (respects the wasm32 ~2 GB cap).
// The JS side (web_fs.js) owns the path->handle index and the block cache.
//
// All symbols are C-linkage so win_kernel.cpp / com_fileaccess.cpp can call them
// from their __EMSCRIPTEN__ branches. The whole file compiles out elsewhere.
//============================================================================
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#if !defined(__EMSCRIPTEN_PTHREADS__)
//============================================================================
// SINGLE-THREAD / FIBER BUILD — Asyncify bridge (unchanged from M4).
//============================================================================
extern "C" {

// Open a file by engine path (Windows-cased, '\\' or '/'); returns an opaque
// positive id, or 0 on failure. Case-insensitive resolution happens in JS.
EM_ASYNC_JS(int, kbweb_open, (const char *cpath), {
    const path = UTF8ToString(cpath);
    if (!Module.KBFS) return 0;
    try { return await Module.KBFS.open(path); }
    catch (e) { console.error("kbweb_open", path, e); return 0; }
});

// Size in bytes of an open file id (or -1).
EM_ASYNC_JS(double, kbweb_size, (int id), {
    if (!Module.KBFS) return -1;
    try { return await Module.KBFS.size(id); }
    catch (e) { console.error("kbweb_size", id, e); return -1; }
});

// SYNCHRONOUS fast path: copies the slice iff every covering block is already
// cached (no Asyncify). Returns bytes copied, or -1 to mean "not cached — use
// the async path". This is what makes the engine's millions of tiny in-block
// reads cheap (a plain JS<->wasm call instead of an Asyncify stack suspend).
EM_JS(int, kbweb_pread_sync, (int id, double offset, void *dst, int len), {
    if (!Module.KBFS || !Module.KBFS.preadCached) return -1;
    const u8 = Module.KBFS.preadCached(id, offset, len);
    if (!u8) return -1;
    HEAPU8.set(u8, dst);
    return u8.length;
});

// Async slice read (cache miss / bulk). Fetches+caches the block(s) in JS, then
// copies into the heap. Suspends/resumes the wasm via Asyncify.
EM_ASYNC_JS(int, kbweb_pread_async, (int id, double offset, void *dst, int len), {
    if (!Module.KBFS) return -1;
    try {
        const u8 = await Module.KBFS.pread(id, offset, len); // Uint8Array
        if (!u8) return 0;
        HEAPU8.set(u8, dst);
        return u8.length;
    } catch (e) { console.error("kbweb_pread", id, offset, len, e); return -1; }
});

// Dispatcher the engine calls: try the sync cache first, fall back to async.
int kbweb_pread(int id, double offset, void *dst, int len) {
    int r = kbweb_pread_sync(id, offset, dst, len);
    if (r >= 0) return r;
    return kbweb_pread_async(id, offset, dst, len);
}

// Close an open id (idempotent).
EM_JS(void, kbweb_close, (int id), {
    if (Module.KBFS) Module.KBFS.close(id);
});

// True if a directory has been granted and the index is ready.
EM_JS(int, kbweb_ready, (void), {
    return (Module.KBFS && Module.KBFS.ready) ? 1 : 0;
});

// Does a path exist (file or dir)? 1=file, 2=dir, 0=absent. Synchronous against
// the prebuilt index (no async needed).
EM_JS(int, kbweb_exists, (const char *cpath), {
    if (!Module.KBFS) return 0;
    return Module.KBFS.exists(UTF8ToString(cpath));
});

// Directory listing for Sys_ListFiles. Synchronous (the index is already in JS
// memory). Returns a malloc'd '\n'-joined UTF8 string of names — the C caller
// must free() it — or 0 if empty.
EM_JS(char *, kbweb_listdir, (const char *cdir, const char *cext, const char *cfilter, int wantsubs), {
    if (!Module.KBFS || !Module.KBFS.listDir) return 0;
    const names = Module.KBFS.listDir(UTF8ToString(cdir), UTF8ToString(cext),
                                      UTF8ToString(cfilter), wantsubs);
    if (!names || !names.length) return 0;
    return stringToNewUTF8(names.join("\n"));
});

} // extern "C"

#else  // __EMSCRIPTEN_PTHREADS__
//============================================================================
// PTHREADS BUILD — main-thread proxy with an atomic doorbell.
//
// The KBFS object (path index, handles, block cache) lives on the DOM thread.
// A worker fills a per-thread request mailbox, posts the work to main with
// MAIN_THREAD_ASYNC_EM_ASM (which returns immediately on the worker), then blocks
// on the mailbox's doorbell word until main wakes it. Because the wasm heap is a
// SharedArrayBuffer, the doorbell store/notify and any bytes the DOM thread copies
// into a dst pointer are visible to the blocked worker with no extra copy.
//
// One mailbox PER calling thread (thread_local) so concurrent worker reads
// (BACKEND vs DATABASE vs STREAM) never clobber each other's request/result.
//============================================================================
#include <emscripten/threading.h>
#include <emscripten/atomic.h>

namespace {
// Doorbell states.
enum { KBW_IDLE = 0, KBW_PENDING = 1, KBW_DONE = 2 };

// Per-worker mailbox. `doorbell` is the futex word the worker waits on; `result`
// carries the int/double return; `dst`/`len` describe the heap copy target.
struct KbwMailbox {
    _Atomic int doorbell;   // KBW_*
    double      result;     // numeric return (size, bytes-read, id, exists, ...)
    void       *dst;
    int         len;
};

// Allocate one mailbox per thread. (thread_local in the shared heap so its address
// is valid to pass to the DOM thread; the DOM thread only writes through it while
// the owning worker is blocked.)
thread_local KbwMailbox g_kbwBox;

// Block the calling worker until the DOM thread sets doorbell != KBW_PENDING.
// emscripten_atomic_wait_u32 is a real futex wait — legal on a pthread/Worker
// (NOT on the browser main thread, which is why main() runs on a proxied worker).
inline void kbw_wait(KbwMailbox *m) {
    while (emscripten_atomic_load_u32(&m->doorbell) == (uint32_t)KBW_PENDING) {
        emscripten_atomic_wait_u32(&m->doorbell, (uint32_t)KBW_PENDING, /*nsec*/ -1);
    }
}

// Worker-local read cache. The engine parses each .iwd's zip central directory with
// hundreds of thousands of TINY sequential reads; proxying every one to the DOM
// thread (a futex round-trip each) takes minutes. So each worker caches the 256 KB
// blocks it has fetched IN ITS OWN JS CONTEXT (globalThis.__kbwB) — repeat reads
// within a block become pure synchronous HEAP copies, never touching main. Only a
// true block miss proxies. A 256 KB thread_local scratch receives a fetched block.
constexpr int KBW_BS = 262144;
thread_local uint8_t g_kbwScratch[KBW_BS];

// Raw slice fetch [offset,len) -> dst on the DOM thread (used to fill a cache block
// and for bulk reads). Blocks the worker on the doorbell. Returns bytes, or -1.
int kbw_proxy_read(int id, double offset, void *dst, int len) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = -1;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const id = $1;
        const offset = $2;
        const dst = $3;
        const len = $4;
        const done = (v) => { HEAPF64[(m + 8) >> 3] = v;
            Atomics.store(HEAP32, m >> 2, 2); Atomics.notify(HEAP32, m >> 2); };
        if (!Module.KBFS) { done(-1); return; }
        try { if (Module.KBFS.preadCached) { const u8 = Module.KBFS.preadCached(id, offset, len);
              if (u8) { HEAPU8.set(u8, dst); done(u8.length); return; } } } catch (e) {}
        Promise.resolve().then(() => Module.KBFS.pread(id, offset, len))
            .then((u8) => { if (!u8) { done(0); return; } HEAPU8.set(u8, dst); done(u8.length); })
            .catch((e) => { console.error("kbweb_pread", id, offset, len, e); done(-1); });
    }, m, id, offset, dst, len);
    kbw_wait(m);
    return (int)m->result;
}
} // namespace

extern "C" {

// Open: proxy to main, which runs KBFS.open(path) (async getFile) and stores the id.
int kbweb_open(const char *cpath) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const cpath = $1;
        const path = UTF8ToString(cpath);
        const done = (val) => { HEAPF64[(m + 8) >> 3] = val;
            Atomics.store(HEAP32, m >> 2, 2 /*DONE*/); Atomics.notify(HEAP32, m >> 2); };
        if (!Module.KBFS) { done(0); return; }
        Promise.resolve().then(() => Module.KBFS.open(path))
            .then((id) => done(id || 0))
            .catch((e) => { console.error("kbweb_open", path, e); done(0); });
    }, m, cpath);
    kbw_wait(m);
    return (int)m->result;
}

// Size: proxy to main (async; getFile().size).
double kbweb_size(int id) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = -1;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const id = $1;
        const done = (val) => { HEAPF64[(m + 8) >> 3] = val;
            Atomics.store(HEAP32, m >> 2, 2); Atomics.notify(HEAP32, m >> 2); };
        if (!Module.KBFS) { done(-1); return; }
        Promise.resolve().then(() => Module.KBFS.size(id))
            .then((sz) => done(sz)).catch((e) => { console.error("kbweb_size", id, e); done(-1); });
    }, m, id);
    kbw_wait(m);
    return m->result;
}

// Synchronous worker-local cache copy. Assembles [offset,len) from cached 256 KB
// blocks held in this worker's JS context. Returns bytes copied (clamped at EOF),
// -1 if any covering block isn't cached (caller fetches + retries), -2 if bulk
// (> one block; not worth caching — proxy a direct slice instead). NO proxying.
EM_JS(int, kbw_local, (int id, double offset, void *dst, int len), {
    const BS = 262144;
    if (len > BS) return -2;
    const g = globalThis.__kbwB; if (!g) return -1;
    const fileMap = g.get(id); if (!fileMap) return -1;
    const end = offset + len;
    let pos = offset;
    let outPos = 0;
    while (pos < end) {
        const bi = Math.floor(pos / BS);
        const bStart = bi * BS;
        const blk = fileMap.get(bi);
        if (!blk) return -1;                       // miss
        const from = pos - bStart;
        if (from >= blk.length) break;             // past EOF inside this (last) block
        const n = Math.min(blk.length - from, end - pos);
        HEAPU8.set(blk.subarray(from, from + n), dst + outPos);
        outPos += n; pos += n;
        if (blk.length < BS) break;                // partial (final) block => EOF
    }
    return outPos;
});

// Store a freshly-fetched aligned block (n bytes in the shared scratch) into this
// worker's private cache, copying it out of the SAB so later reads are stable.
EM_JS(void, kbw_store, (int id, int blockIdx, void *scratch, int n), {
    const g = (globalThis.__kbwB || (globalThis.__kbwB = new Map()));
    let fileMap = g.get(id);
    if (!fileMap) { fileMap = new Map(); g.set(id, fileMap); }
    fileMap.set(blockIdx, HEAPU8.slice(scratch, scratch + n));   // private (non-shared) copy
    if (fileMap.size > 64) fileMap.delete(fileMap.keys().next().value);  // ~16MB/file cap
});

// Pread: serve from the worker-local block cache when possible (pure sync heap copy,
// no thread hop); on a miss, fetch the covering aligned 256 KB block(s) from the DOM
// thread once, cache them, and retry. Bulk reads bypass the cache (direct proxy).
int kbweb_pread(int id, double offset, void *dst, int len) {
    int r = kbw_local(id, offset, dst, len);
    if (r >= 0) return r;
    if (r == -2) return kbw_proxy_read(id, offset, dst, len);   // bulk: uncached slice
    // Cache miss: fetch the covering block(s) (a small read spans at most two).
    long long bi0 = (long long)(offset / (double)KBW_BS);
    double end = offset + (double)len;
    for (long long bi = bi0; (double)bi * (double)KBW_BS < end; ++bi) {
        double bStart = (double)bi * (double)KBW_BS;
        int n = kbw_proxy_read(id, bStart, g_kbwScratch, KBW_BS);
        if (n < 0) return (bStart <= offset) ? n : 0;   // error on the first block
        if (n == 0) break;                              // EOF at/after this block
        kbw_store(id, (int)bi, g_kbwScratch, n);
        if (n < KBW_BS) break;                          // last (partial) block
    }
    r = kbw_local(id, offset, dst, len);
    return r >= 0 ? r : 0;
}

// Close: synchronous on main (no async; just drops the handle).
void kbweb_close(int id) {
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.KBFS) Module.KBFS.close($0); }, id);
}

// ready/exists are synchronous against the in-JS index, but that index lives on the
// DOM thread. Proxy them too (they're rare relative to reads) so a worker sees the
// granted folder. We block on the doorbell exactly like the async ops.
int kbweb_ready(void) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const val = (Module.KBFS && Module.KBFS.ready) ? 1 : 0;
        HEAPF64[(m + 8) >> 3] = val;
        Atomics.store(HEAP32, m >> 2, 2); Atomics.notify(HEAP32, m >> 2);
    }, m);
    kbw_wait(m);
    return (int)m->result;
}

int kbweb_exists(const char *cpath) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const cpath = $1;
        const val = Module.KBFS ? Module.KBFS.exists(UTF8ToString(cpath)) : 0;
        HEAPF64[(m + 8) >> 3] = val;
        Atomics.store(HEAP32, m >> 2, 2); Atomics.notify(HEAP32, m >> 2);
    }, m, cpath);
    kbw_wait(m);
    return (int)m->result;
}

// Directory listing: proxy to main, which builds the '\n'-joined name list against
// the index and copies it into a wasm-heap buffer it mallocs (via _malloc) — the C
// caller free()s it, same contract as the Asyncify build. We pass back the pointer
// through the mailbox result (as a double-encoded integer; pointers are < 2^53).
char *kbweb_listdir(const char *cdir, const char *cext, const char *cfilter, int wantsubs) {
    KbwMailbox *m = &g_kbwBox;
    emscripten_atomic_store_u32(&m->doorbell, (uint32_t)KBW_PENDING);
    m->result = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const m = $0;
        const cdir = $1;
        const cext = $2;
        const cfilter = $3;
        const wantsubs = $4;
        let ptr = 0;
        try {
            if (Module.KBFS && Module.KBFS.listDir) {
                const names = Module.KBFS.listDir(UTF8ToString(cdir), UTF8ToString(cext),
                                                  UTF8ToString(cfilter), wantsubs);
                if (names && names.length) ptr = stringToNewUTF8(names.join("\n"));
            }
        } catch (e) { console.error("kbweb_listdir", e); }
        HEAPF64[(m + 8) >> 3] = ptr;
        Atomics.store(HEAP32, m >> 2, 2); Atomics.notify(HEAP32, m >> 2);
    }, m, cdir, cext, cfilter, wantsubs);
    kbw_wait(m);
    return (char *)(uintptr_t)m->result;
}

} // extern "C"

#endif // __EMSCRIPTEN_PTHREADS__
#endif // __EMSCRIPTEN__
