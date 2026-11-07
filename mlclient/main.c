/*
 * main.c - native CLI over libmlengine.so (bring-up / test harness).
 *
 * All the real work lives in the engine .so (mlengine.h / engine_api.c); this just
 * parses argv, starts a session, waits, and stops cleanly on the host drop or a
 * signal. The .NET host drives the same four ml_engine_* functions.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mlengine.h"

static volatile sig_atomic_t g_quit;

static void on_sig(int s) { (void)s; g_quit = 1; }

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
      "usage: %s <host-ip> <appid> [width=1920] [height=1080] [fps=60] [bitrate_kbps=20000]\n"
      "  env: MOONLIGHT_PIN (4 digits, first-time pairing), MOONLIGHT_DATADIR (default /tmp)\n",
      argv[0]);
    return 2;
  }
  const char *host = argv[1];
  int appId  = atoi(argv[2]);
  int width  = argc > 3 ? atoi(argv[3]) : 0;
  int height = argc > 4 ? atoi(argv[4]) : 0;
  int fps    = argc > 5 ? atoi(argv[5]) : 0;
  int bitrate= argc > 6 ? atoi(argv[6]) : 0;
  const char *pin = getenv("MOONLIGHT_PIN");
  const char *datadir = getenv("MOONLIGHT_DATADIR");

  /* the CLI runs no ecore main loop: a rebuild's display step would never run */
  setenv("MLENGINE_PLAYER_REBUILD_MAX", "0", 0);

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  int rc = ml_engine_start(host, appId, pin, width, height, fps, bitrate, datadir);
  if (rc != 0) { fprintf(stderr, "[ml] start failed rc=%d\n", rc); return rc == 2 ? 2 : 1; }

  fprintf(stderr, "[ml] streaming; Ctrl+C to stop\n");
  while (!g_quit && ml_engine_running()) { struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL); }
  ml_engine_stop();
  return 0;
}
