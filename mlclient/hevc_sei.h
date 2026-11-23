/*
 * hevc_sei.h - diagnostic scan of an Annex-B buffer for HEVC prefix SEI NAL units (header-only, pure C).
 * Not a parser: emulation prevention bytes are ignored; every read is bounded by the buffer.
 */
#ifndef HEVC_SEI_H
#define HEVC_SEI_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Returns the number of prefix SEI NALs (type 39) in p[0..n); *mask |= 1 for payload 137 (mastering display), |= 2 for 144 (content light level). */
static inline int hevc_sei_scan(const uint8_t *p, size_t n, int *mask) {
  int cnt = 0;
  for (size_t i = 0; i + 3 < n; i++) {
    if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
    size_t s = i + 3, e = s;   /* the NAL is [s, e): up to the next 00 00 01 or the end */
    while (e + 2 < n && !(p[e] == 0 && p[e + 1] == 0 && p[e + 2] == 1)) e++;
    if (e + 2 >= n) e = n;
    if (s + 2 <= e && ((p[s] >> 1) & 0x3f) == 39) {
      cnt++;
      size_t q = s + 2;
      while (q + 2 <= e && p[q] != 0x80) {
        unsigned type = 0, size = 0;
        while (q < e && p[q] == 0xFF) { type += 255; q++; }
        if (q >= e) break;
        type += p[q++];
        while (q < e && p[q] == 0xFF) { size += 255; q++; }
        if (q >= e) break;
        size += p[q++];
        if (type == 137) *mask |= 1;
        if (type == 144) *mask |= 2;
        if (size > e - q) break;
        q += size;
      }
    }
    i = e > i + 2 ? e - 1 : i + 2;   /* the next loop step lands on e (the next start code) */
  }
  return cnt;
}

#define HEVC_SEI_MAX 64   /* start code + header + payloads + emulation prevention bytes, with room to spare */

/* Builds one prefix SEI NAL (start code, 137 mastering display, 144 content light level). Returns the bytes written, 0 if cap is too small.
   prim_rgb_xy: R x,y G x,y B x,y (0.00002 units); the SEI wants G, B, R. max_lum_nits is scaled to 0.0001 cd/m2; min is already in that unit. */
static inline size_t hevc_sei_build(uint8_t *out, size_t cap, const uint16_t prim_rgb_xy[6], uint16_t wp_x, uint16_t wp_y,
                                    uint32_t max_lum_nits, uint32_t min_lum_1e4nits, uint16_t max_cll, uint16_t max_fall) {
  uint8_t r[40]; size_t n = 0;
  const int order[3] = {1, 2, 0};   /* G, B, R */
  const uint8_t head[] = {0x4E, 0x01, 137, 24};
  memcpy(r, head, sizeof head); n = sizeof head;
#define PUT16(v) do { r[n++] = (uint8_t)((v) >> 8); r[n++] = (uint8_t)(v); } while (0)
#define PUT32(v) do { PUT16((uint32_t)(v) >> 16); PUT16((uint32_t)(v) & 0xFFFF); } while (0)
  for (int c = 0; c < 3; c++) { PUT16(prim_rgb_xy[2 * order[c]]); PUT16(prim_rgb_xy[2 * order[c] + 1]); }
  PUT16(wp_x); PUT16(wp_y); PUT32(max_lum_nits * 10000u); PUT32(min_lum_1e4nits);
  r[n++] = 144; r[n++] = 4; PUT16(max_cll); PUT16(max_fall);
#undef PUT32
#undef PUT16
  r[n++] = 0x80;
  if (cap < 4) return 0;
  size_t o = 4, z = 0;
  out[0] = out[1] = out[2] = 0; out[3] = 1;
  for (size_t i = 0; i < n; i++) {
    if (z >= 2 && r[i] <= 3) { if (o >= cap) return 0; out[o++] = 3; z = 0; }
    if (o >= cap) return 0;
    out[o++] = r[i];
    z = r[i] ? 0 : z + 1;
  }
  return o;
}
#endif
