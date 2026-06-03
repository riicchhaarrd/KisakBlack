// web_fs.h — C interface to the File System Access bridge (web_fs.cpp).
// Emscripten only; see web_fs.cpp for the design.
#ifndef KISAK_WEB_FS_H
#define KISAK_WEB_FS_H
#ifdef __EMSCRIPTEN__
#ifdef __cplusplus
extern "C" {
#endif
int    kbweb_open(const char *path);                 // -> id>0, or 0 on failure
double kbweb_size(int id);                            // bytes, or -1
int    kbweb_pread(int id, double offset, void *dst, int len); // bytes read, or -1
void   kbweb_close(int id);
int    kbweb_ready(void);                             // 1 once a dir is granted
int    kbweb_exists(const char *path);                // 0 absent, 1 file, 2 dir
#ifdef __cplusplus
}
#endif
#endif // __EMSCRIPTEN__
#endif
