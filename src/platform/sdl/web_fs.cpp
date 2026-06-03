//============================================================================
// web_fs.cpp  (Emscripten only)
//
// Synchronous-looking file bridge over the Chrome File System Access API.
//
// The engine reads game data synchronously (FS_FileRead/fread and the
// db_file_load ReadFileEx path). The File System Access API (showDirectoryPicker
// -> FileSystemFileHandle.getFile() -> Blob.arrayBuffer()) is ASYNC. Since the
// web build is single-threaded (M3), we bridge with **Asyncify**: each call here
// uses EM_ASYNC_JS, which suspends the wasm stack, performs the async JS read,
// and resumes — so the C side sees a blocking call.
//
// Files are streamed on demand (never fully resident): kbweb_pread reads exactly
// the requested [offset,len) slice from the underlying Blob via Blob.slice, so a
// multi-GB .ff is never brought into the wasm heap (respects the wasm32 ~2 GB
// cap). The JS side (web_fs.js) owns the path->handle index and a small LRU of
// open File objects.
//
// All symbols are C-linkage so win_kernel.cpp / com_fileaccess.cpp can call them
// from their __EMSCRIPTEN__ branches. The whole file compiles out elsewhere.
//============================================================================
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

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

// Read up to `len` bytes at absolute `offset` into wasm memory at `dst`.
// Returns bytes actually read (>=0), or -1 on error. Streams the slice — only
// the requested window is materialized in JS, then copied into the heap.
EM_ASYNC_JS(int, kbweb_pread, (int id, double offset, void *dst, int len), {
    if (!Module.KBFS) return -1;
    try {
        const u8 = await Module.KBFS.pread(id, offset, len); // Uint8Array
        if (!u8) return 0;
        HEAPU8.set(u8, dst);
        return u8.length;
    } catch (e) { console.error("kbweb_pread", id, offset, len, e); return -1; }
});

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

} // extern "C"

#endif // __EMSCRIPTEN__
