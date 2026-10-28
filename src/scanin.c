/* Copyright (C) 2026 heydemoura */
#include "scanin.h"
#include "log.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* IPMI::Client vtable offsets on this firmware (read from libSceAvcap2):
 *   +0x00 destructor, +0x18 disconnect, +0x60 invokeSyncMethod, +0x80 connect */
#define VT_INVOKE   0x60
#define VT_CONNECT  0x80

/* Mangled IPMI symbols (dlsym hashes the name to the module's NID). */
#define SYM_CFG_CTOR  "_ZN4IPMI6Client6ConfigC1Ev"
#define SYM_CFG_SIZE  "_ZN4IPMI6Client6Config24estimateClientMemorySizeEv"
#define SYM_CREATE    "_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_"

typedef struct { void *p; uint64_t len; } data_info;

static void    *g_client;           /* IPMI::Client* */

static int
ipmi_setup(void) {
  if (g_client) return 0;

  void *lib = dlopen("libSceIpmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) lib = dlopen("/system/common/lib/libSceIpmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) { LOG("scanin: dlopen libSceIpmi failed: %s", dlerror()); return -1; }

  void  (*cfg_ctor)(void *)       = dlsym(lib, SYM_CFG_CTOR);
  uint64_t (*cfg_size)(void *)    = dlsym(lib, SYM_CFG_SIZE);
  int   (*cli_create)(void **, const void *, void *, void *) = dlsym(lib, SYM_CREATE);
  if (!cfg_ctor || !cfg_size || !cli_create) {
    LOG("scanin: IPMI symbols missing (ctor=%p size=%p create=%p)",
        (void*)cfg_ctor, (void*)cfg_size, (void*)cli_create);
    return -1;
  }

  /* Build the client config: default-construct, then name the daemon and set
   * the queue sizes (mirrors how the system sets up this client). */
  uint8_t cfg[0x200];
  memset(cfg, 0, sizeof(cfg));
  cfg_ctor(cfg);
  memset(cfg, 0, 32);
  strcpy((char *)cfg, "SceSysAvControlIpc");       /* name field at offset 0 */
  *(uint32_t *)(cfg + 0x1c) = 1;
  *(uint32_t *)(cfg + 0x20) = 1;
  *(uint64_t *)(cfg + 0x28) = 0x400;
  *(uint64_t *)(cfg + 0x30) = 0x400;
  *(uint32_t *)(cfg + 0x38) = 2;
  *(uint64_t *)(cfg + 0x40) = 0x400;
  *(uint64_t *)(cfg + 0x48) = 0x400;

  uint64_t need = cfg_size(cfg);
  if (!need || need > 0x100000) need = 0x10000;
  void *mem = malloc(need);
  if (!mem) return -1;
  memset(mem, 0, need);

  void *client = NULL;
  int r = cli_create(&client, cfg, NULL, mem);
  if (r || !client) { LOG("scanin: Client::create -> 0x%x", r); free(mem); return -1; }
  g_client = client;

  void **vt = *(void ***)client;
  int (*connect)(void *, int, int, void *, long *) =
      (int (*)(void *, int, int, void *, long *))vt[VT_CONNECT / 8];
  long timeout = 20000000;
  r = connect(client, 0, 0, cfg, &timeout);
  LOG("scanin: connect(SceSysAvControlIpc) -> 0x%x", r);
  if (r) { g_client = NULL; return r; }
  return 0;
}

int
scanin_enable(int res_index) {
  if (ipmi_setup() != 0) return -1;

  void **vt = *(void ***)g_client;
  int (*invoke)(void *, unsigned, const data_info *, unsigned, void *, void *) =
      (int (*)(void *, unsigned, const data_info *, unsigned, void *, void *))vt[VT_INVOKE / 8];

  uint32_t scanin_id = 1;
  int32_t  params[8];
  memset(params, 0, sizeof(params));
  params[0] = 2;            /* tap point (capture tap)      */
  params[1] = 1;
  params[2] = 0x18;
  params[3] = res_index;    /* 11 = 1920x1080               */
  params[4] = 1;
  params[5] = 1;
  params[6] = 0;

  data_info data[2];
  data[0].p = &scanin_id; data[0].len = 4;
  data[1].p = params;     data[1].len = 0x20;

  uint64_t out1[4] = {0}, out2[4] = {0};
  int r = invoke(g_client, 0x24 /* set scan-in */, data, 2, out1, out2);
  LOG("scanin: invoke(setScanIn res=%d) -> 0x%x (out=0x%llx)",
      res_index, r, (unsigned long long)out1[0]);
  return r;
}
