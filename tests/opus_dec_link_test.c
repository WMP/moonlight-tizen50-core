/* Links against the decoder-only libopus list (opus_sources DIR dec) and decodes: lost packets (PLC) and a CELT packet. */
#include <stdio.h>
#include <opus_multistream.h>

int main(void) {
  unsigned char map[2] = { 0, 1 };
  int err = 0;
  OpusMSDecoder *d = opus_multistream_decoder_create(48000, 2, 1, 1, map, &err);
  if (!d || err != OPUS_OK) return 1;
  opus_int16 pcm[240 * 2];
  /* TOC 0xE0 = CELT fullband, 2.5 ms, one frame: 120 samples */
  unsigned char pkt[8] = { 0xE0, 0, 0, 0, 0, 0, 0, 0 };
  int n = opus_multistream_decode(d, pkt, sizeof pkt, pcm, 240, 0);
  int m = opus_multistream_decode(d, NULL, 0, pcm, 240, 0);
  opus_multistream_decoder_destroy(d);
  if (n != 120 || m != 240) { printf("opus_dec_link_test FAILED n=%d m=%d\n", n, m); return 2; }
  puts("opus_dec_link_test OK");
  return 0;
}
