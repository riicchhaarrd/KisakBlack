#include "com_fileaccess.h"
#include <qcommon/com_profilemapload.h>
#include <string.h>
#include "assertive.h"
#include "q_shared.h"

#ifdef __EMSCRIPTEN__
// ----- File System Access bridge for the stdio data path (IWDs, configs, …) ---
// The decompiled engine reads all data files through this small wrapper layer
// (FileWrapper_Open / FS_FileRead / FS_FileClose / FileWrapper_Seek /
// FileWrapper_GetFileSize), and unzip.cpp routes its ZIP_f* through the same
// wrappers. So intercepting them here covers the whole stdio read path. A
// read-only open against the granted Steam folder returns an opaque WebFile*
// disguised as a FILE*; a registry distinguishes web handles from real FILE*s so
// writes/temp files still use Emscripten MEMFS. Reads stream on demand via
// kbweb_pread (only the requested window is materialized — never the whole .ff).
#include <platform/sdl/web_fs.h>
#include <set>
#include <cstdlib>
namespace {
struct WebFile { int id; long long pos; long long size; };
std::set<FILE *> &webSet() { static std::set<FILE *> s; return s; }
bool isWeb(FILE *f) { return f && webSet().count(f) != 0; }
FILE *webOpen(const char *path) {
    if (!kbweb_ready()) return nullptr;
    int id = kbweb_open(path);
    if (id <= 0) return nullptr;
    WebFile *w = (WebFile *)malloc(sizeof(WebFile));
    w->id = id; w->pos = 0; w->size = (long long)kbweb_size(id);
    FILE *h = (FILE *)w; webSet().insert(h);
    return h;
}
} // namespace
#endif

unsigned int __cdecl FS_FileRead(void *ptr, unsigned int len, FILE *stream)
{
    unsigned int read_size; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_READ);
#ifdef __EMSCRIPTEN__
    if (isWeb(stream)) {
        WebFile *w = (WebFile *)stream;
        int r = kbweb_pread(w->id, (double)w->pos, ptr, (int)len);
        if (r < 0) r = 0;
        w->pos += r;
        ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_READ);
        return (unsigned int)r;
    }
#endif
    read_size = fread(ptr, 1u, len, stream);
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_READ);
    return read_size;
}

unsigned int __cdecl FS_FileWrite(const void *ptr, unsigned int len, FILE *stream)
{
    return fwrite(ptr, 1u, len, stream);
}

FILE *__cdecl FileWrapper_Open(const char *ospath, const char *mode)
{
    int *v3; // eax
    const char *v4; // eax
    const char *v5; // eax
    FILE *file; // [esp+0h] [ebp-4h]

#ifdef __EMSCRIPTEN__
    // Read-only opens ("rb"/"rt"/"r") -> File System Access bridge (the granted
    // Steam folder). Write/append modes fall through to MEMFS.
    if (mode && (mode[0] == 'r')) {
        FILE *w = webOpen(ospath);
        if (w) return w;
        // not found in the granted folder; fall through to MEMFS read (rare)
    }
#endif
    *_errno() = 0;
    file = fopen(ospath, mode);
    if ( file != (FILE *)-1 )
        return file;
    v3 = _errno();
    v4 = strerror(*v3);
    v5 = va("Couldn't open file: %s %s", ospath, v4);
    if ( !Assert_MyHandler(
                    "c:\\projects_pc\\cod\\codsrc\\src\\qcommon\\../universal/com_files_wrapper_stdio.h",
                    24,
                    0,
                    v5) )
        __debugbreak();
    return 0;
}

FILE *__cdecl FS_FileOpenReadBinary(const char *filename)
{
    FILE *file; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FileWrapper_Open(filename, "rb");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *__cdecl FS_FileOpenReadText(const char *filename)
{
    FILE *file; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FileWrapper_Open(filename, "rt");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *__cdecl FS_FileOpenWriteBinary(const char *filename)
{
    FILE *file; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FileWrapper_Open(filename, "wb");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *__cdecl FS_FileOpenAppendText(const char *filename)
{
    FILE *file; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FileWrapper_Open(filename, "at");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *__cdecl FS_FileOpenWriteText(const char *filename)
{
    FILE *file; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FileWrapper_Open(filename, "w+t");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

void __cdecl FS_FileClose(FILE *stream)
{
#ifdef __EMSCRIPTEN__
    if (isWeb(stream)) {
        WebFile *w = (WebFile *)stream;
        kbweb_close(w->id);
        webSet().erase(stream);
        free(w);
        return;
    }
#endif
    fclose(stream);
}

int __cdecl FS_FileSeek(FILE *file, int offset, int whence)
{
    int seek; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_SEEK);
    seek = FileWrapper_Seek(file, offset, whence);
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_SEEK);
    return seek;
}

int __cdecl FileWrapper_Seek(FILE *h, int offset, int origin)
{
    const char *v4; // eax

#ifdef __EMSCRIPTEN__
    // Web handle: same origin remap as the stdio path below (0->CUR, 1->END,
    // 2->SET). Returns 0 on success, like fseek.
    if (isWeb(h)) {
        WebFile *w = (WebFile *)h;
        long long base = (origin == 0) ? w->pos : (origin == 1) ? w->size : 0;
        long long np = base + offset;
        if (np < 0) return -1;
        w->pos = np;
        return 0;
    }
#endif
    switch ( origin )
    {
        case 0:
            return fseek(h, offset, 1);
        case 1:
            return fseek(h, offset, 2);
        case 2:
            return fseek(h, offset, 0);
    }
    v4 = va("Bad origin %i in FS_Seek", origin);
    if ( !Assert_MyHandler(
                    "c:\\projects_pc\\cod\\codsrc\\src\\qcommon\\../universal/com_files_wrapper_stdio.h",
                    111,
                    0,
                    v4) )
        __debugbreak();
    return 0;
}

int __cdecl FS_FileGetFileSize(FILE *file)
{
    return FileWrapper_GetFileSize(file);
}

int __cdecl FileWrapper_GetFileSize(FILE *h)
{
    int startPos; // [esp+0h] [ebp-8h]
    int fileSize; // [esp+4h] [ebp-4h]

#ifdef __EMSCRIPTEN__
    if (isWeb(h)) return (int)((WebFile *)h)->size;
#endif
    startPos = ftell(h);
    fseek(h, 0, 2);
    fileSize = ftell(h);
    fseek(h, startPos, 0);
    return fileSize;
}

#ifdef __EMSCRIPTEN__
// ftell() that understands both a File System Access web handle and a real FILE*.
long FileWrapper_Tell(FILE *h)
{
    if (isWeb(h)) return (long)((WebFile *)h)->pos;
    return ftell(h);
}
#endif

