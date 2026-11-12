/* Host self-test for mlclient/engine_avoc.c. dlopen/dlsym/dlclose are renamed to the fakes below
 * (-Ddlopen=fake_dlopen ...). With an argument it runs the "library missing" case. Run via tests/build-esplus-ecore-test.sh. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

int ml_avoc_set_game_mode(int on);
int ml_engine_tv_game_mode(int on, int save_type);

static int g_lib = 1, g_rc, g_rc1, g_calls, g_type, g_mode;   /* g_rc1: rc for save_type 1 only (0 = use g_rc) */
static int g_log[16][2];   /* every (save_type, mode) call */

static int fake_avoc_set_game_mode(int save_type, int mode) {
  g_log[g_calls][0] = save_type; g_log[g_calls][1] = mode;
  g_calls++; g_type = save_type; g_mode = mode;
  return save_type == 1 && g_rc1 ? g_rc1 : g_rc;
}
/* only libavoc.so is faked: with -DMLT_TRACE mltrace.h also looks up dlog_print */
void *fake_dlopen(const char *name, int flags) { (void)flags; return g_lib && !strcmp(name, "libavoc.so") ? (void *)&g_lib : NULL; }
void *fake_dlsym(void *h, const char *name) { (void)h; return !strcmp(name, "avoc_set_game_mode") ? (void *)fake_avoc_set_game_mode : NULL; }
int fake_dlclose(void *h) { (void)h; return 0; }

int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1) {   /* library missing: both calls return -1 (the lookup result is cached per process) */
    g_lib = 0;
    assert(ml_avoc_set_game_mode(0) == -1 && ml_avoc_set_game_mode(1) == -1);
    assert(ml_engine_tv_game_mode(0, 1) == -1 && g_calls == 0);
    puts("engine_avoc_test (no library) OK");
    return 0;
  }
  /* fresh process: off reaches the TV (g_on starts at "never"), both calls in order */
  assert(ml_avoc_set_game_mode(0) == 0);
  assert(g_calls == 2 && g_log[0][0] == 1 && g_log[0][1] == 0 && g_log[1][0] == 0 && g_log[1][1] == 0);
  assert(ml_avoc_set_game_mode(0) == 0 && g_calls == 2);   /* already off: skipped */

  /* on twice: one TV call, (1,1) */
  assert(ml_avoc_set_game_mode(1) == 0 && ml_avoc_set_game_mode(1) == 0);
  assert(g_calls == 3 && g_log[2][0] == 1 && g_log[2][1] == 1);

  /* immediate call: always reaches the TV, save_type passes through */
  assert(ml_engine_tv_game_mode(1, 1) == 0);
  assert(g_calls == 4 && g_type == 1 && g_mode == 1);
  assert(ml_avoc_set_game_mode(1) == 0 && g_calls == 4);   /* session logic saw the on */
  assert(ml_engine_tv_game_mode(0, 1) == 0 && g_calls == 5);
  assert(ml_avoc_set_game_mode(0) == 0 && g_calls == 7);   /* a manual single off is not a full off */
  assert(ml_engine_tv_game_mode(1, 1) == 0 && g_calls == 8);
  assert(ml_engine_tv_game_mode(0, 0) == 0 && g_calls == 9 && g_type == 0 && g_mode == 0);
  assert(ml_avoc_set_game_mode(0) == 0 && g_calls == 11);

  /* first call fails: the second is still sent, rc is the first failure, the next off retries */
  assert(ml_avoc_set_game_mode(1) == 0 && g_calls == 12);
  g_rc1 = -5;
  assert(ml_avoc_set_game_mode(0) == -5 && g_calls == 14 && g_log[13][0] == 0 && g_log[13][1] == 0);
  g_rc1 = 0;
  assert(ml_avoc_set_game_mode(0) == 0 && g_calls == 16);

  /* the TV's rc is returned */
  g_rc = -3;
  assert(ml_engine_tv_game_mode(1, 2) == -3);
  assert(g_type == 2 && g_mode == 1);
  puts("engine_avoc_test OK");
  return 0;
}
