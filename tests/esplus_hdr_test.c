/* Host test of mlclient/esplus_hdr.c: the colour text and the apply chain, with fake trackrenderer symbols.
   Built with -Ddlopen=fake_dlopen -Ddlsym=fake_dlsym. "nolib" as argument: dlopen fails. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esplus_hdr.h"

static char fake_obj[0x1C0], fake_adapter[16];
static void *got_tr; static char got_txt[600]; static int set_calls, set_ret, tr_time_ret, state_ret = 7, state_calls;
static uint64_t tr_ms, pl_ms; static int pl_ok;
static int fake_set(void *tr, const char *t) { got_tr = tr; snprintf(got_txt, sizeof got_txt, "%s", t); set_calls++; return set_ret; }
static int fake_time(void *tr, uint64_t *t) { (void)tr; *t = tr_ms; return tr_time_ret; }
static int fake_state(void *tr) { (void)tr; state_calls++; return state_ret; }
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *t) { (void)h; *t = pl_ms; return pl_ok; }
static int lib_ok = 1, no_state;
void *fake_dlopen(const char *n, int f) { (void)f; return lib_ok && !strcmp(n, "libtrackrenderer.so") ? (void *)&lib_ok : NULL; }   /* MLT_TRACE also dlopens libdlog */
void *fake_dlsym(void *l, const char *n) {
  (void)l;
  if (!strcmp(n, "trackrenderer_set_matroska_color_info")) return (void *)fake_set;
  if (!strcmp(n, "trackrenderer_get_playing_time")) return (void *)fake_time;
  if (!strcmp(n, "trackrenderer_get_state")) return no_state ? NULL : (void *)fake_state;
  return NULL;
}
int fake_dlclose(void *l) { (void)l; return 0; }

static void put(size_t off, void *p) { memcpy(fake_obj + off, &p, sizeof p); }

int main(int argc, char **argv) {
  const uint16_t prim[6] = {34000, 16000, 13250, 34500, 7500, 3000};
  char t[600];
  size_t n = hdr_color_text(t, sizeof t, prim, 15635, 16450, 1000, 50, 800, 400, 0);
  const char *want = "matrixCoefficients:9 bitsPerChannel:10 chromaSubsamplingHorz:1 chromaSubsamplingVert:1 cbSubsamplingHorz:0 cbSubsamplingVert:0 "
    "chromaSitingHorz:1 chromaSitingVert:2 range:1 transferCharacteristics:16 primaries:9 maxCLL:800 maxFALL:400 "
    "RX:0.68000 RY:0.32000 GX:0.26500 GY:0.69000 BX:0.15000 BY:0.06000 wX:0.31270 wY:0.32900 luminanceMax:1000.0000 luminanceMin:0.0050 isHDR10p:0";
  assert(n == strlen(want) && !strcmp(t, want));
  {
    unsigned a[13]; double d[10]; int hp = -1;
    int k = sscanf(t, "matrixCoefficients:%u bitsPerChannel:%u chromaSubsamplingHorz:%u chromaSubsamplingVert:%u cbSubsamplingHorz:%u cbSubsamplingVert:%u "
        "chromaSitingHorz:%u chromaSitingVert:%u range:%u transferCharacteristics:%u primaries:%u maxCLL:%u maxFALL:%u "
        "RX:%lf RY:%lf GX:%lf GY:%lf BX:%lf BY:%lf wX:%lf wY:%lf luminanceMax:%lf luminanceMin:%lf isHDR10p:%d",
        &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &a[9], &a[10], &a[11], &a[12],
        &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7], &d[8], &d[9], &hp);
    assert(k == 24);
    const unsigned wa[13] = {9, 10, 1, 1, 0, 0, 1, 2, 1, 16, 9, 800, 400};
    for (int i = 0; i < 13; i++) assert(a[i] == wa[i]);
    const double wd[10] = {0.68, 0.32, 0.265, 0.69, 0.15, 0.06, 0.3127, 0.329, 1000.0, 0.005};
    for (int i = 0; i < 10; i++) assert(d[i] > wd[i] - 1e-9 && d[i] < wd[i] + 1e-9);
    assert(hp == 0);
  }
  hdr_color_text(t, sizeof t, prim, 15635, 16450, 1000, 50, 800, 400, 1); assert(strstr(t, " range:2 "));
  assert(hdr_color_text(t, 50, prim, 15635, 16450, 1000, 50, 800, 400, 0) == 0);
  assert(hdr_color_text(t, n, prim, 15635, 16450, 1000, 50, 800, 400, 0) == 0 && hdr_color_text(t, n + 1, prim, 15635, 16450, 1000, 50, 800, 400, 0) == n);

  esplus_hdr_diag d;
  if (argc > 1 && !strcmp(argv[1], "nostate")) {   /* the optional symbol is missing: not an error */
    no_state = 1; put(0x1B4, fake_adapter); { void *tr = fake_obj + 8; memcpy(fake_adapter, &tr, sizeof tr); }
    tr_ms = pl_ms = 1; pl_ok = 1;
    assert(esplus_hdr_apply((esplusplayer_handle)fake_obj, "x", &d) == 1 && d.sym_state == 0 && d.state == -99 && state_calls == 0);
    puts("hdr nostate ok"); return 0; }
  if (argc > 1) { lib_ok = 0; assert(esplus_hdr_apply((esplusplayer_handle)fake_obj, "x", &d) == -1);
    assert(d.lib == 0 && d.sym_set == 0 && d.adapter == '-' && !strcmp(d.steps, "---")); puts("hdr nolib ok"); return 0; }

  esplusplayer_handle h = (esplusplayer_handle)fake_obj;
  tr_ms = pl_ms = 5000; pl_ok = 1; tr_time_ret = 0; set_ret = 0;
  put(0x1B4, NULL);   /* adapter NULL */
  assert(esplus_hdr_apply(h, "txt", &d) == -2 && set_calls == 0 && d.adapter == 'n' && d.handle == '-' && !strcmp(d.steps, "n--"));
  assert(d.lib == 1 && d.sym_set == 1 && d.sym_time == 1 && d.sym_state == 1 && d.state == -99 && d.set_rc == -99);
  assert(esplus_hdr_apply(NULL, "txt", &d) == -2 && d.adapter == 'n');
  { long pg = sysconf(_SC_PAGESIZE);   /* an unmapped page: the probe must say unreadable, not crash */
    char *gone = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(gone != MAP_FAILED);
    munmap(gone, 2 * pg);
    assert(esplus_hdr_apply((esplusplayer_handle)(gone), "txt", &d) == -2 && d.adapter == 'u' && !strcmp(d.steps, "u--"));
    put(0x1B4, gone + pg);
    assert(esplus_hdr_apply(h, "txt", &d) == -2 && d.adapter == 'o' && d.handle == 'u' && !strcmp(d.steps, "ou-") && set_calls == 0); }
  put(0x1B4, fake_adapter);
  { void *z = NULL; memcpy(fake_adapter, &z, sizeof z); }   /* tr NULL */
  assert(esplus_hdr_apply(h, "txt", &d) == -2 && d.handle == 'n' && !strcmp(d.steps, "on-"));
  { void *tr = fake_obj + 8; memcpy(fake_adapter, &tr, sizeof tr); }
  assert(esplus_hdr_apply(h, "hello", &d) == 1 && set_calls == 1 && got_tr == fake_obj + 8 && !strcmp(got_txt, "hello"));
  assert(!strcmp(d.steps, "ooo") && d.tr_rc == 0 && d.pl_rc == 0 && d.tr_ms == 5000 && d.pl_ms == 5000 && d.state == 7 && d.set_rc == 0 && state_calls == 1);
  pl_ms = 5099; assert(esplus_hdr_apply(h, "a", &d) == 1);   /* inside the limit */
  { int c = set_calls, sc = state_calls;
    pl_ms = 5100; assert(esplus_hdr_apply(h, "a", &d) == -3 && !strcmp(d.steps, "oom") && d.tr_ms == 5000 && d.pl_ms == 5100 && d.state == -99 && d.set_rc == -99);
    pl_ms = 4900; assert(esplus_hdr_apply(h, "a", &d) == -3);
    pl_ms = 5000; tr_time_ret = -1; assert(esplus_hdr_apply(h, "a", &d) == -3 && d.tr_rc == -1); tr_time_ret = 0;
    pl_ok = 0; assert(esplus_hdr_apply(h, "a", &d) == -3 && d.pl_rc == -1); pl_ok = 1;
    assert(set_calls == c && state_calls == sc); }   /* a failed chain check: no state read, no set */
  { int c = set_calls; set_ret = -1; assert(esplus_hdr_apply(h, "a", &d) == -4 && set_calls == c + 1 && d.set_rc == -1 && !strcmp(d.steps, "ooo")); set_ret = 0; }
  puts("hdr ok"); return 0;
}
