/*
 * wma_decode.h - Minimal standalone C API around the Rockbox libwma
 * (integer WMAv1/WMAv2) decoder, for the KisakBlack engine port.
 *
 * No Rockbox framework dependency. Pure C, safe for native gcc and
 * emscripten alike.
 */
#ifndef WMA_DECODE_H
#define WMA_DECODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WmaDecoder WmaDecoder;

/*
 * Open a decoder.
 *
 * channels, sampleRate, blockAlign, avgBytesPerSec come straight from the
 * WAVEFORMATEX of the WMA stream.  extraData / extraLen are the format
 * extension bytes (cbSize/extradata); they may be NULL/0 for xWMA streams
 * that carry no extradata.
 *
 * Only WMAv2 (codec tag 0x161) is targeted; codec_id is forced to 0x161.
 *
 * Returns a decoder handle, or NULL on failure.
 */
WmaDecoder *wma_open(int channels, int sampleRate, int blockAlign,
                     int avgBytesPerSec, const unsigned char *extraData,
                     int extraLen);

/*
 * Decode ONE WMA packet/block (typically blockAlign bytes).
 *
 * Writes interleaved signed 16-bit PCM to 'out', which has capacity
 * 'outCapSamples' total int16 elements (i.e. frames * channels).
 *
 * Returns the number of int16 samples written (>= 0), or -1 on error.
 */
int wma_decode_packet(WmaDecoder *d, const unsigned char *packet,
                      int packetLen, short *out, int outCapSamples);

/* Free a decoder. NULL-safe. */
void wma_close(WmaDecoder *d);

#ifdef __cplusplus
}
#endif

#endif /* WMA_DECODE_H */
