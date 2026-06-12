/*
 * rbglue.h - Standalone glue layer for the Rockbox libwma (integer WMA)
 * decoder, used inside the KisakBlack engine port.
 *
 * This header REPLACES the Rockbox framework headers <codecs.h> and
 * <codecs/lib/codeclib.h>.  It provides:
 *   - empty definitions for all the IRAM / section-placement attribute
 *     macros the vendored sources expect,
 *   - a no-op DEBUGF,
 *   - codec_malloc/calloc/realloc/free mapped to the standard malloc family
 *     (WITHOUT globally redefining malloc),
 *   - av_log2 (an integer floor(log2) bit-length helper),
 *   - little-endian configuration (target is wasm/x86),
 *   - the real "mdct.h" and "fft.h" so the decoder sees ff_imdct_calc().
 *
 * It is intentionally self-contained: no Rockbox framework dependency.
 */
#ifndef RBGLUE_H
#define RBGLUE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/* Endianness: target is wasm32 / x86, always little-endian.          */
/* codeclib_misc.h, ffmpeg_get_bits.h and wmafixed.c branch on these. */
/* ------------------------------------------------------------------ */
#ifndef ROCKBOX_LITTLE_ENDIAN
#define ROCKBOX_LITTLE_ENDIAN 1
#endif
/* Deliberately do NOT define ROCKBOX_BIG_ENDIAN. */

/* ------------------------------------------------------------------ */
/* IRAM / section placement attributes -> all empty on this target.   */
/* ------------------------------------------------------------------ */
#ifndef ICODE_ATTR
#define ICODE_ATTR
#endif
#ifndef IBSS_ATTR
#define IBSS_ATTR
#endif
#ifndef ICONST_ATTR
#define ICONST_ATTR
#endif
#ifndef IDATA_ATTR
#define IDATA_ATTR
#endif
#ifndef MEM_ALIGN_ATTR
#define MEM_ALIGN_ATTR
#endif
#ifndef NO_PROF_ATTR
#define NO_PROF_ATTR
#endif
#ifndef ICODE_ATTR_TREMOR_MDCT
#define ICODE_ATTR_TREMOR_MDCT
#endif
/* NOTE: the WMA-specific *_WMA_LARGE_IRAM / *_WMA_XL_IRAM macros are
 * defined by wmadec.h's no-CPU_* fallback branch, and HAVE_ATTRIBUTE_PACKED
 * is defined by ffmpeg_intreadwrite.h; we deliberately leave both to those
 * owners to avoid -W redefinition warnings.
 *
 * Branch-prediction hints used by mdct.c / fft-ffmpeg.c. */
#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* ------------------------------------------------------------------ */
/* DEBUGF -> no-op.                                                    */
/* ------------------------------------------------------------------ */
#ifndef DEBUGF
#define DEBUGF(...) do {} while (0)
#endif

/* ------------------------------------------------------------------ */
/* codec_* allocator shims -> standard malloc family.                 */
/* NOTE: we do NOT '#define malloc codec_malloc' (the real codeclib.h  */
/* does that, but it pollutes every TU); the decoder is fully static   */
/* and does not actually allocate, so these exist only to satisfy any  */
/* references.                                                         */
/* ------------------------------------------------------------------ */
static inline void *codec_malloc(size_t size)              { return malloc(size); }
static inline void *codec_calloc(size_t nmemb, size_t size){ return calloc(nmemb, size); }
static inline void *codec_realloc(void *ptr, size_t size)  { return realloc(ptr, size); }
static inline void  codec_free(void *ptr)                  { free(ptr); }

/* ------------------------------------------------------------------ */
/* av_log2: integer floor(log2(x)) == bit length - 1. av_log2(0)==0.   */
/* Replaces the bs_generic()/bs_log2_tab machinery from codeclib.h.    */
/* ------------------------------------------------------------------ */
#ifndef av_log2
static inline unsigned int av_log2(unsigned int v)
{
    unsigned int n = 0;
    if (v == 0) return 0;
    while (v >>= 1) n++;
    return n;
}
#endif

/* ------------------------------------------------------------------ */
/* byte-swap helpers referenced by ffmpeg_bswap.h / ffmpeg_get_bits.h */
/* (only used on the big-endian path, but must exist as symbols).     */
/* ------------------------------------------------------------------ */
#ifndef swap16
static inline uint16_t swap16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}
#endif
#ifndef swap32
static inline uint32_t swap32(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) <<  8) |
           ((x & 0x00FF0000u) >>  8) |
           ((x & 0xFF000000u) >> 24);
}
#endif

/* ------------------------------------------------------------------ */
/* The decoder expects ff_imdct_calc / ff_imdct_half / FFTComplex from */
/* the real mdct/fft headers.                                          */
/* ------------------------------------------------------------------ */
#include "fft.h"
#include "mdct.h"

#endif /* RBGLUE_H */
