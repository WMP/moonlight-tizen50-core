/* Host self-test for mlclient/engine_api.c (start_session, game mode, ML-ERROR for bad settings, host masking).
   Limelight, the sinks and the host calls are stubs. Run via tests/build-esplus-ecore-test.sh. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "Limelight.h"
#include "errors.h"
#include "esplus_stats.h"
#include "mlengine.h"

esplus_stats_t esplus_stats;
uint32_t EncryptionFeaturesSupported, EncryptionFeaturesRequested, EncryptionFeaturesEnabled;

static int avoc_on, avoc_off, start_calls;
static unsigned char iv0[4];
static CONNECTION_LISTENER_CALLBACKS lcb;
int ml_engine_audio_route_native(void) { return 0; }   /* audio_opus.c is not linked here */
int ml_avoc_set_game_mode(int on) { if (on) avoc_on++; else avoc_off++; return 0; }
void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb) { memset(cb, 0, sizeof *cb); }
void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb) { memset(cb, 0, sizeof *cb); }
int input_init(void) { return 0; }
void input_shutdown(void) {}
int ml_cert_init(const char *dir) { (void)dir; return 0; }
int gs_orchestrate(const char *h, unsigned short a, unsigned short b, int c, const char *p,
                   STREAM_CONFIGURATION *sc, SERVER_INFORMATION *si, const char *d) {
  (void)h;(void)a;(void)b;(void)c;(void)p;(void)sc;(void)si;(void)d; return 0;
}
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION s) { memset(s, 0, sizeof *s); }
void LiInitializeServerInformation(PSERVER_INFORMATION s) { memset(s, 0, sizeof *s); }
void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS c) { memset(c, 0, sizeof *c); }
int LiStartConnection(PSERVER_INFORMATION si, PSTREAM_CONFIGURATION sc, PCONNECTION_LISTENER_CALLBACKS cl,
                      PDECODER_RENDERER_CALLBACKS dr, PAUDIO_RENDERER_CALLBACKS ar, void *dc, int df, void *ac, int af) {
  (void)si;(void)dr;(void)ar;(void)dc;(void)df;(void)ac;(void)af;
  start_calls++; lcb = *cl; memcpy(iv0, sc->remoteInputAesIv, 4); return 0;
}
void LiStopConnection(void) {}
const char *LiGetStageName(int s) { (void)s; return "stage"; }
bool LiGetEstimatedRttInfo(uint32_t *r, uint32_t *v) { *r = *v = 0; return false; }
uint32_t LiGetHostFeatureFlags(void) { return 0; }
bool LiGetHdrMetadata(PSS_HDR_METADATA m) { m->maxDisplayLuminance = 1000; return true; }

#define KEY "00112233445566778899aabbccddeeff"
static int start(const char *key, int w, int h) {
  return ml_engine_start_session_v2("192.168.1.20", w, h, 60, 20000, key, 0x01020304, "7.1.431.-1", "3.23.0.74",
                                    NULL, 0, "H264", 0, "Stereo", 0);
}

int main(void) {
  char buf[2048];
  /* (a) game mode on: set before start, on while streaming, off at stop */
  ml_engine_set_gamemode(1);
  assert(start(KEY, 1280, 720) == 0 && avoc_on == 1);
  ml_engine_stop(); assert(avoc_off >= 1);
  /* (d) rikeyid is the IV's first 4 bytes, big-endian */
  assert(iv0[0] == 1 && iv0[1] == 2 && iv0[2] == 3 && iv0[3] == 4);
  /* (b) game mode off: never on */
  ml_engine_set_gamemode(0); avoc_on = 0;
  assert(start(KEY, 1280, 720) == 0 && avoc_on == 0);
  /* (e) the host address never reaches the ML-ERROR line */
  lcb.logMessage("No working addresses found for host: 192.168.1.20\n");
  lcb.stageFailed(1, -1);
  assert(ml_engine_last_error(buf, sizeof buf) != 0);
  assert(strstr(buf, "<host>") && !strstr(buf, "192.168.1.20"));
  ml_engine_stop();
  /* (c) bad settings: GS_INVALID and one ML-ERROR line with the requested size */
  assert(start("0011", 1280, 720) == GS_INVALID);
  assert(ml_engine_last_error(buf, sizeof buf) != 0 && strstr(buf, "stage=settings") && strstr(buf, "cfg 1280x720@60"));
  assert(start("zz112233445566778899aabbccddeeff", 1280, 720) == GS_INVALID);
  assert(ml_engine_last_error(buf, sizeof buf) != 0 && strstr(buf, "stage=settings") && strstr(buf, "cfg 1280x720@60"));
  assert(start(KEY, 100, 100) == GS_INVALID);
  assert(ml_engine_last_error(buf, sizeof buf) != 0 && strstr(buf, "stage=settings") && strstr(buf, "cfg 100x100@60"));
  puts("engine api ok"); return 0;
}
