/*
 * _wma_selftest.c - smoke test for the standalone WMA decoder.
 *
 * Opens a decoder with plausible WMAv2 parameters, feeds one packet of
 * zeros, and prints the returned sample count. The point is to prove the
 * library compiles, links, and runs without crashing.
 */
#include <stdio.h>
#include "wma_decode.h"

int main(void)
{
    const int channels   = 2;
    const int sampleRate = 44100;
    const int blockAlign = 4096;
    const int avgBytes   = 6000 * 2;   /* ~96 kbps */

    WmaDecoder *d = wma_open(channels, sampleRate, blockAlign, avgBytes,
                             NULL, 0);
    if (!d) {
        printf("wma_open FAILED\n");
        return 1;
    }
    printf("wma_open OK (ch=%d rate=%d blockAlign=%d)\n",
           channels, sampleRate, blockAlign);

    /* One packet of zeros. */
    static unsigned char packet[4096];   /* zero-initialised */
    static short out[8 * 2048 * 2];      /* generous PCM capacity */

    int n = wma_decode_packet(d, packet, (int)sizeof(packet),
                              out, (int)(sizeof(out) / sizeof(out[0])));
    printf("wma_decode_packet returned %d int16 samples (%d frames)\n",
           n, n >= 0 ? n / channels : -1);

    wma_close(d);
    printf("wma_close OK; selftest done\n");
    return n < 0 ? 1 : 0;
}
