// gl_optrace.h — last-N GL-layer operations ring, dumped when the context's GPU
// channel dies (the canary flip). Names the exact neighborhood of the killing call
// instead of guessing from the last console print.
#ifndef KISAK_GL_OPTRACE_H
#define KISAK_GL_OPTRACE_H

#ifdef __cplusplus
extern "C" {
#endif
// tag MUST be a string literal (the pointer is stored, not the contents).
void KB_OpTag(const char *tag, unsigned a, unsigned b, unsigned c);
void KB_DumpOpRing(void);   // print the ring oldest->newest (once per run)
#ifdef __cplusplus
}
#endif

#endif
