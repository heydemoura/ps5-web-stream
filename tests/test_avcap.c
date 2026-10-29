/* Host regression for native API sequencing, cleanup and packet metadata. */
#include <assert.h>
#include "../src/avcap.c"
char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
int scanin_enable(int index) { (void)index; assert(0); return -1; }
static int initialized, terminated, starts, stops, closes, running, registered;
static int open_error, start_error, header_error, read_result = 5, executable = 1;
static int key_requests, key_error, next_idr = 1;
static uint32_t expected_resolution = 4, expected_bitrate = 8000000, expected_fps = 60;
static uint64_t next_pts = UINT64_C(240000)*20000000+4004;
static uint64_t now_tsc = UINT64_C(240000)*20000000+4004;
static uint64_t read_tsc(void) { return now_tsc; }
static void (*callback)(int,int,void *);
static uint32_t get32(const void *p, size_t off) {
  uint32_t v; memcpy(&v,(const uint8_t *)p+off,4); return v;
}
static int init(void) { ++initialized; return 0; }
static int term(void) { ++terminated; return 0; }
static int mode(int *out) { assert(initialized); *out=0x13c; return 0; }
static int ra_set(int id, void (*fn)(int,int,void *), void *arg) {
  assert(id==0xb4 && !arg); registered=1; callback=fn; return 0;
}
static int ra_check(int id,const char *state) {
  assert(id==0xb4 && !strcmp(state,"RUNNING")); return executable;
}
static int ra_state(int id,const char *state,void *arg,int pid) {
  assert(id==0xb4 && !arg && pid==getpid());
  running=!strcmp(state,"RUNNING"); uint8_t success=1; callback(id,1,&success); return 0;
}
static int ra_clear(int id) { assert(id==0xb4); registered=0; return 0; }
static int open_video(void **handle,void *p,int flag) {
  assert(!flag && running);
  assert(get32(p,0)==0x2e0 && get32(p,8)==0x28);
  assert(get32(p,0x14)==2 && get32(p,0x18)==expected_resolution &&
         get32(p,0x1c)==(expected_fps==60));
  assert(get32(p,0x24)==0 && get32(p,0x28)==0xb4 && get32(p,0x2a0)==0x13c);
  assert(get32(p,0xcc)==expected_bitrate &&
         get32(p,0xd0)==(uint64_t)expected_bitrate*1001/(expected_fps*1000));
  assert(get32(p,0x5c)==(expected_resolution==6 ? 4u : 9u));
  if (open_error) return open_error;
  *handle=(void *)(uintptr_t)1; return 0;
}
static int headers(void *h,void *out) {
  assert(h && get32(out,0)==0x1a8);
  if (header_error) return header_error;
  uint8_t *p=out; uint64_t n=1; memcpy(p+0x10,&n,8);p[0x18]=0x67;
  memcpy(p+0x98,&n,8);p[0xa0]=0x68; return 0;
}
static int start(void *h) { assert(h); ++starts; return start_error; }
static int stop(void *h) { assert(h); ++stops; return 0; }
static int close_video(void *h) { assert(h); ++closes; return 0; }
static int request_key(void *h) { assert(h); ++key_requests; return key_error; }
static int read_video(void *h,void *out,uint64_t cap,void *info,int flags) {
  assert(h && flags==1 && cap>=5);
  if (read_result<=0) return read_result;
  memcpy(out,"\0\0\0\1\x65",5);
  ((uint8_t *)out)[4] = next_idr ? 0x65 : 0x41;
  uint64_t pts=next_pts;
  uint64_t dts=pts-4004;
  memcpy((uint8_t *)info+8,&pts,8);memcpy((uint8_t *)info+16,&dts,8);
  return read_result;
}
int main(void) {
  g_lib=g_ra_lib=(void *)(uintptr_t)1;
  p_Initialize=init;p_Terminate=term;p_GetVideoOutMode=mode;p_OpenVideo=open_video;
  p_GetHeaders=headers;p_Start=start;p_Stop=stop;p_Close=close_video;p_ReadVideo=read_video;
  p_RequestKeyFrame=request_key;
  p_ReadTsc=read_tsc; g_tsc_frequency=240000;
  p_RaSet=ra_set;p_RaCheck=ra_check;p_RaState=ra_state;p_RaClear=ra_clear;
  avcap_config cfg={1920,1080,60,8000,0};
  assert(avcap_open(&cfg)==AVCAP_INVALID);
  assert(avcap_init()==0 && avcap_init()==0 && initialized==1);
  assert(avcap_open(&cfg)==0);
  assert(avcap_open(&cfg)==AVCAP_INVALID); /* active handle cannot be overwritten */
  uint8_t buf[512];avcap_frame frame;
  read_result=AVCAP_EMPTY;assert(avcap_read_video(buf,sizeof(buf),&frame)==0);
  read_result=AVCAP_OVERRUN;assert(avcap_read_video(buf,sizeof(buf),&frame)==0);
  read_result=AVCAP_INVALID;assert(avcap_read_video(buf,sizeof(buf),&frame)==AVCAP_INVALID);
  read_result=5;assert(avcap_read_video(buf,sizeof(buf),&frame)==1);
  assert(frame.keyframe && frame.len==15);
  assert(buf[4]==0x67 && buf[9]==0x68 && buf[14]==0x65);
  assert(frame.pts_us==UINT64_C(20000000016683) && frame.dts_us==UINT64_C(20000000000000));
  assert(avcap_read_video(buf,5,&frame)==AVCAP_INVALID); /* no header-buffer overflow */
  next_idr=0; next_pts+=479999; /* Just before two seconds: no request. */
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1 && !frame.keyframe && frame.len==5);
  assert(key_requests==0);
  ++next_pts;
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1 && key_requests==1);
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1 && key_requests==1);
  next_idr=1;next_pts+=4004;
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1 && frame.keyframe && frame.len==15);
  assert(key_requests==1); /* Arrival resets the next request deadline. */
  next_idr=0;next_pts+=480000;key_error=(int)0x81950008;
  assert(avcap_read_video(buf,sizeof(buf),&frame)==key_error);
  assert(key_requests==2 && avcap_last_error()==key_error);key_error=0;
  avcap_close();assert(!g_video && !running && registered && stops==1 && closes==1);
  assert(!terminated && g_initialized); /* audio still owns shared service */
  open_error=(int)0x81950008;assert(avcap_open(&cfg)==open_error);
  assert(!running && registered && closes==1);open_error=0;
  header_error=(int)0x81950001;assert(avcap_open(&cfg)==header_error);
  assert(!running && registered && closes==2);header_error=0;
  start_error=(int)0x80c78000;assert(avcap_open(&cfg)==start_error);
  assert(!running && registered && closes==3);start_error=0;
  executable=2;running=1;assert(avcap_open(&cfg)==0);
  now_tsc += UINT64_C(240000)*60;
  next_pts+=480000;
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1 && key_requests==2);
  assert(frame.pts_us == (now_tsc/240000)*1000000 + (now_tsc%240000)*1000000/240000);
  uint64_t first = frame.pts_us;
  now_tsc += 12345; next_pts += 4004;
  assert(avcap_read_video(buf,sizeof(buf),&frame)==1);
  assert(frame.pts_us-first == 16683); /* delivery jitter never replaces source cadence */
  /* Reopening resets the request clock even if capture time advanced. */
  avcap_close();
  assert(running && registered); /* never STOP a state this client did not acquire */
  executable=1; running=0;
  static const avcap_config modes[] = {
    {1280,720,60,5000,0}, {1920,1080,60,8000,0},
    {2560,1440,60,16000,0}, {3840,2160,30,32000,0}
  };
  for (unsigned i=0; i<4; i++) {
    expected_resolution=3+i; expected_bitrate=(uint32_t)modes[i].bitrate*1000;
    expected_fps=(uint32_t)modes[i].fps;
    assert(avcap_open(&modes[i])==0);
    avcap_close(); assert(!running && registered);
  }
  avcap_terminate();avcap_terminate();assert(terminated==1 && !registered);
  puts("avcap: sequencing, RA cleanup, nonblocking status, headers, keyframe requests and timestamps pass");
  return 0;
}
