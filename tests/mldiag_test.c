/* Host self-test for mlclient/mldiag.c. Run via tests/build-esplus-ecore-test.sh, or:
   gcc -std=gnu11 -Wall -Wextra -Werror -Imlclient -o /tmp/mldiag_test tests/mldiag_test.c mlclient/mldiag.c -lpthread && /tmp/mldiag_test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mldiag.h"

static const mldiag_cfg CFG = { 1920, 1080, 60, 20000, 1392, 1, 1, 0, 0x30002, 1, 0, "7.1.431.-1", "3.23.0.74", NULL, 0, NULL, NULL };

static void fail(int code) { mldiag_fail(code, -1, NULL, 0, 2, 1, 0, 0, 0, 0, 0); }

int main(void) {
  char buf[4096];
  mldiag_reset(&CFG);
  assert(mldiag_last(buf, sizeof buf) == 0 && buf[0] == 0);

  /* counters + first_pkt_ms */
  mldiag_cc_line("Received first video packet after 12 ms\n");
  mldiag_cc_line("Unrecoverable frame 5\n");
  mldiag_cc_line("Network dropped 3 frames\n");
  mldiag_cc_line("Waiting for IDR frame\n");
  mldiag_cc_line("Depacketizer detected corrupt frame\n");
  mldiag_cc_line("Recovery error\n");
  mldiag_cc_line("Requesting IDR frame on behalf of video decoder\n");
  mldiag_cc_line("Sending RFI request\n");
  mldiag_cc_line("Recovered 2 packets\n");
  mldiag_cc_line("Video decode unit queue overflow\n");
  mldiag_video_setup(1, 1920, 1080, 60, 0);
  mldiag_video_du(0); mldiag_video_du(1);
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);   /* prep not reached: all times "-" */
  assert(strstr(buf, " | prep-t pdone=- err=- v1=- a1=- vpts1=- apts1=- | "));
  mldiag_player("open", 1); mldiag_player("win", 0); mldiag_player("prep", 1); mldiag_player("pdone", 1); mldiag_player("start", 1);
  mldiag_submit(1, 1, 4, 100); mldiag_submit(0, 1, 4, 50); mldiag_submit(0, 0, 3, 60); mldiag_submit(0, 0, 0, 70);   /* 0 = NOT_PREPARED: a failure */
  mldiag_player("state", 2); mldiag_player("err", 0); mldiag_player("err", 0);
  mldiag_fail(-101, -1, NULL, 0, 2, 1, 7, 9, 0x7, 0x0, 0x1);
  assert(mldiag_last(buf, sizeof buf) == -101);
  assert(strncmp(buf, "ML-ERROR code=-101 name=NO_VIDEO_FRAME stage=- t=", 49) == 0);
  assert(strstr(buf, "cfg 1920x1080@60 br=20000 pkt=1392 fmts=0x1 cs=1 cr=0 audio=0x30002 scm=0x1 enc=0x0 encf=0x7/0x0/0x1 rtsp=- launch=- rikey=- rikeyid=- app=7.1.431.-1 gfe=3.23.0.74 hostflags=0x0 rtt=2/1"));
  assert(strstr(buf, "first_pkt_ms=12"));
  assert(strstr(buf, "du=2 idr=1"));
  assert(strstr(buf, "open=1 win=0 prep=1 pdone=1 start=1 state=2 rebuilds=0 rb=-99/-99/-99/-99 vsub=1 asub=3 subfail=2/0 vfail=0/0 afail=2/0 adec=0 adec_fail=0/0 err=7/9"));
  { int a, b, c, d; assert(sscanf(strstr(buf, "prep-t pdone="), "prep-t pdone=%d err=%d v1=%d a1=%d vpts1=100 apts1=50 |", &a, &b, &c, &d) == 4);
    assert(a >= 0 && b >= 0 && c >= 0 && d >= 0 && a < 1000); }
  assert(strstr(buf, "cc unrec=1 drop=1 waitidr=1 badlen=0 decrypt=0 corrupt=2 unkft=0 idrreq=2 fec=1 qovf=1 droplim=0"));
  assert(!strchr(buf, '\n'));
  puts(buf);

  mldiag_fail(-110, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf); assert(strstr(buf, "code=-110 name=PLAYER_NOT_STARTED "));
  mldiag_fail(-111, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf); assert(strstr(buf, "code=-111 name=PLAYER_STALLED "));

  /* rtsp scheme shows in the cfg section */
  { mldiag_cfg rc = CFG; rc.rtsp_scheme = "rtspenc"; mldiag_reset(&rc); fail(-101); mldiag_last(buf, sizeof buf);
    assert(strstr(buf, "rtsp=rtspenc launch=")); }

  /* IDR request and control decrypt messages (the #13 case) */
  mldiag_reset(&CFG);
  mldiag_cc_line("IDR frame request sent\n");
  mldiag_cc_line("Failed to decrypt control packet of size 40\n");
  mldiag_cc_line("Discarding unencrypted packet on encrypted control stream: 0302\n");
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "decrypt=2 ") && strstr(buf, "idrreq=1 "));

  /* name mapping for an unknown code; stage name */
  mldiag_reset(&CFG);
  mldiag_fail(-7, 3, "RTSP handshake", 0, 0, 0, 0, 0, 0, 0, 0);
  assert(mldiag_last(buf, sizeof buf) == -7);
  assert(strstr(buf, "name=OTHER stage=RTSP handshake"));

  /* first 4 / last 8 ring with 20 lines */
  mldiag_reset(&CFG);
  for (int i = 0; i < 20; i++) { char l[32]; snprintf(l, sizeof l, "line%02d\n", i); mldiag_cc_line(l); }
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "cc-first: line00 ; line01 ; line02 ; line03 | cc-last: line12 ; line13 ; line14 ; line15 ; line16 ; line17 ; line18 ; line19"));

  /* fewer than 4 lines */
  mldiag_reset(&CFG);
  mldiag_cc_line("only"); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "cc-first: only | cc-last: only"));

  /* very long cc line: truncated to 160, whole buffer bounded */
  mldiag_reset(&CFG);
  char big[5000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
  for (int i = 0; i < 20; i++) mldiag_cc_line(big);
  fail(-101);
  assert(mldiag_last(buf, sizeof buf) == -101);
  assert(strlen(buf) < 2048);
  char x161[162], x160[162]; memset(x161, 'x', 161); x161[161] = 0; memcpy(x160, x161, 162); x160[160] = 0;
  assert(strstr(buf, x160) && !strstr(buf, x161));   /* each cc line cut to 160 chars */

  /* small len, NULL buf */
  char small[10]; memset(small, 'z', sizeof small);
  assert(mldiag_last(small, sizeof small) == -101);
  assert(strlen(small) == 9 && strncmp(small, "ML-ERROR ", 9) == 0);
  assert(mldiag_last(NULL, 100) == -101);
  assert(mldiag_last(buf, 0) == -101);

  /* launch / resume key check; the note is consumed by one reset */
  static const unsigned char KEY[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
  unsigned char key2[16]; memcpy(key2, KEY, 16); key2[15] ^= 1;
  mldiag_cfg kc = CFG; kc.aes_key = KEY; kc.rikeyid = -5;
  mldiag_note_launch("https://h:47984/resume?uniqueid=x&rikey=00112233445566778899AABBCCDDEEFF&rikeyid=-5");
  mldiag_reset(&kc); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "launch=resume rikey=ok rikeyid=ok "));
  assert(!strstr(buf, "00112233") && !strstr(buf, "aabbcc"));
  mldiag_reset(&kc); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "launch=- rikey=- rikeyid=- "));
  kc.aes_key = key2; kc.rikeyid = 7;
  mldiag_note_launch("https://h/launch?rikey=00112233445566778899aabbccddeeff&rikeyid=-5&x=1");
  mldiag_reset(&kc); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "launch=launch rikey=DIFF rikeyid=DIFF "));
  mldiag_note_launch("https://h/launch?rikey=0011&rikeyid=1");
  mldiag_reset(&kc); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "launch=launch rikey=bad rikeyid=DIFF "));
  mldiag_note_launch("https://h/launch?rikey=zz112233445566778899aabbccddeeff");
  mldiag_reset(&kc); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "rikey=bad rikeyid=- "));

  /* the host address is masked in the cc lines (also when it is cut by the line limit) */
  { mldiag_cfg hc = CFG; hc.host = "192.168.1.20"; mldiag_reset(&hc);
    mldiag_cc_line("No working addresses found for host: 192.168.1.20\n");
    mldiag_cc_line("a 192.168.1.20 b 192.168.1.20\n");
    char big2[256]; memset(big2, 'x', 155); strcpy(big2 + 155, "192.168.1.20\n"); mldiag_cc_line(big2);
    fail(-101); mldiag_last(buf, sizeof buf);
    assert(strstr(buf, "No working addresses found for host: <host>") && strstr(buf, "a <host> b <host>"));
    assert(!strstr(buf, "192.168") && !strstr(buf, "1.20"));
    hc.host = ""; mldiag_reset(&hc); mldiag_cc_line("host 192.168.1.20\n"); fail(-101); mldiag_last(buf, sizeof buf);
    assert(strstr(buf, "host 192.168.1.20") && !strstr(buf, "<host>"));
    mldiag_reset(&CFG); mldiag_cc_line("host 192.168.1.20\n"); fail(-101); mldiag_last(buf, sizeof buf);
    assert(strstr(buf, "host 192.168.1.20")); }

  /* control stream encrypt counters */
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | ctl enc=0 fail=0 iv=- verify=0/0/0 "));
  mldiag_ctl_encrypt(1, 12, 1); mldiag_ctl_encrypt(1, 12, 1); mldiag_ctl_encrypt(1, 16, 0); mldiag_ctl_encrypt(0, 16, -1); mldiag_ctl_encrypt(1, 12, -1);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | ctl enc=5 fail=1 iv=12 verify=2/1/2 "));
  puts(buf);

  /* pad counters */
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | pad arr=0 st=0 fail=0/0 | cc "));
  mldiag_pad(1, 0); mldiag_pad(0, 0); mldiag_pad(0, -1); mldiag_pad(1, 5); mldiag_pad(0, 0);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | pad arr=2 st=3 fail=2/5 | cc "));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | pad arr=0 st=0 fail=0/0 | cc "));

  /* audio decode counters */
  mldiag_reset(&CFG);
  mldiag_adec(1, 0); mldiag_adec(1, 0); mldiag_adec(0, -4); mldiag_adec(0, -2);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " adec=2 adec_fail=2/-2 err="));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " adec=0 adec_fail=0/0 err="));

  /* audio output, IDR retries */
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " idr_retry=0 | player "));
  assert(strstr(buf, " | aout none reason=- | ctl "));
  mldiag_aout(0);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout none reason=no-callback | ctl "));
  mldiag_aout(1); mldiag_aframe(960); mldiag_aframe(960); mldiag_adrops(3); mldiag_idr_retry(); mldiag_idr_retry();
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " idr_retry=2 | player "));
  assert(strstr(buf, " | aout webaudio frames=2 bytes=1920 sent=0 drops=3 sendfail=0 | ctl "));
  mldiag_adrops(0);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout webaudio frames=2 bytes=1920 sent=0 drops=0 sendfail=0 | ctl "));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " idr_retry=0 | player ") && strstr(buf, " | aout none reason=- | ctl "));

  /* native audio-io output (audio_opus.c): counters, and the fallback variant */
  mldiag_reset(&CFG);
  mldiag_aout_native(1); mldiag_anative(100, 2, 3, 4, 55);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout native frames=100 underruns=2 overruns=3 drops=4 buf_ms=55 | ctl "));
  mldiag_anative(200, 0, 0, 0, 60);   /* read time: the worker's last report wins */
  mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout native frames=200 underruns=0 overruns=0 drops=0 buf_ms=60 | ctl "));
  mldiag_aout_native(0);   /* native failed: fell back to the callback path */
  mldiag_aout_reason("create--13");
  mldiag_aframe(960); mldiag_ahost(5, 1, 0);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout webaudio fallback=native reason=create--13 frames=1 bytes=960 sent=5 drops=1 sendfail=0 | ctl "));
  mldiag_last(buf, sizeof buf);   /* reason survives a re-render */
  assert(strstr(buf, "reason=create--13"));
  mldiag_aout_native(1); mldiag_anative(7, 1, 0, 0, 48);   /* a later session decides again */
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout native frames=7 underruns=1 overruns=0 drops=0 buf_ms=48 | ctl "));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout none reason=- | ctl "));

  /* player rebuilds */
  mldiag_reset(&CFG);
  mldiag_rebuild(); mldiag_rebuild();
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " state=-99 rebuilds=2 rb=-99/-99/-99/-99 vsub="));
  mldiag_player("rb_open", 1); mldiag_player("rb_addv", 1); mldiag_player("rb_disp", 0);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " rebuilds=2 rb=1/1/0/-99 vsub="));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " state=-99 rebuilds=0 rb=-99/-99/-99/-99 vsub="));

  /* catch-up seek */
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " err=0/0 catchup=0/-/-"));
  mldiag_catchup(730, 1);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " catchup=1/730/1"));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " catchup=0/-/-"));

  /* stats overlay: want survives mldiag_reset, the window result does not */
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " catchup=0/-/- osd=0/- |"));
  mldiag_osd(1, 1); fail(-101); mldiag_last(buf, sizeof buf); assert(strstr(buf, " osd=1/lowered"));
  mldiag_osd(0, 2); fail(-101); mldiag_last(buf, sizeof buf); assert(strstr(buf, " osd=0/raised"));
  mldiag_osd(1, -1); fail(-101); mldiag_last(buf, sizeof buf); assert(strstr(buf, " osd=1/nosym"));
  mldiag_reset(&CFG);
  fail(-101); mldiag_last(buf, sizeof buf); assert(strstr(buf, " osd=1/-"));
  mldiag_osd(0, 0);

  /* the host reports its audio counters after the failure: mldiag_last shows the values at read time, the rest is unchanged */
  mldiag_reset(&CFG); mldiag_aout(1); mldiag_aframe(960);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | aout webaudio frames=1 bytes=960 sent=0 drops=0 sendfail=0 | ctl "));
  mldiag_ahost(40, 3, 2); mldiag_adrops(4);
  { char b2[4096]; mldiag_last(b2, sizeof b2);
    assert(strstr(b2, " | aout webaudio frames=1 bytes=960 sent=40 drops=4 sendfail=2 | ctl "));
    assert(strstr(b2, "cc-first: ") && strstr(b2, "cc-last: ") && strncmp(b2, buf, 20) == 0); }
  mldiag_reset(&CFG);
  mldiag_ahost(1, 1, 1); fail(-101); mldiag_last(buf, sizeof buf);   /* aout never reached: stays none */
  assert(strstr(buf, " | aout none reason=- | ctl "));
  mldiag_reset(&CFG); mldiag_aout(1);
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "sent=0 drops=0 sendfail=0 | ctl "));   /* reset clears the host counters */

  /* reset clears */
  mldiag_reset(&CFG);
  assert(mldiag_last(buf, sizeof buf) == 0 && buf[0] == 0);
  puts("mldiag_test OK");
  return 0;
}
