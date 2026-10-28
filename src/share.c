/* Copyright (C) 2026 heydemoura */
#include "share.h"
#include "log.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

static int (*p_permit)(int);
static int (*p_ex_permit)(int);
static int (*p_init)(uint64_t, int, void *);

static int
share_load(void) {
  if (p_permit) return 0;
  void *lib = dlopen("libSceShare.native.sprx", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) lib = dlopen("/system/common/lib/libSceShare.native.sprx", RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) { LOG("share: dlopen libSceShare failed: %s", dlerror()); return -1; }
  p_permit    = dlsym(lib, "sceShareFeaturePermit");
  p_ex_permit = dlsym(lib, "sceShareFeatureExPermit");
  p_init      = dlsym(lib, "sceShareInitialize");
  if (!p_permit) { LOG("share: sceShareFeaturePermit not found: %s", dlerror()); return -1; }

  if (p_init) {
    int uid = -1;
    void *us = dlopen("libSceUserService.sprx", RTLD_LAZY | RTLD_GLOBAL);
    if (!us) us = dlopen("/system/common/lib/libSceUserService.sprx", RTLD_LAZY | RTLD_GLOBAL);
    if (us) {
      int (*us_init)(void *) = dlsym(us, "sceUserServiceInitialize");
      int (*get_user)(int *) = dlsym(us, "sceUserServiceGetInitialUser");
      int (*get_list)(int *) = dlsym(us, "sceUserServiceGetLoginUserIdList");
      if (us_init) { int ir = us_init(NULL); LOG("share: UserServiceInitialize -> 0x%x", ir); }
      if (get_list) {
        int list[4] = { -1, -1, -1, -1 };
        int lr = get_list(list);
        LOG("share: LoginUserIdList -> 0x%x [%d %d %d %d]", lr, list[0], list[1], list[2], list[3]);
        if (list[0] > 0) uid = list[0];
      }
      if (uid < 0 && get_user) { int gr = get_user(&uid); LOG("share: GetInitialUser -> 0x%x uid=%d", gr, uid); }
    }
    int r = p_init((uint64_t)0x100000, uid, NULL);
    LOG("share: sceShareInitialize(1MB, uid=%d) -> 0x%x", uid, r);
  } else {
    LOG("share: sceShareInitialize not found");
  }
  return 0;
}

int
share_permit(int feature_id) {
  if (share_load() != 0) return -1;
  int r = p_permit(feature_id);
  if (p_ex_permit) p_ex_permit(feature_id);
  return r;
}

int
share_permit_sweep(int lo, int hi) {
  if (share_load() != 0) return 0;
  int n = 0;
  for (int id = lo; id <= hi; id++) {
    int r = p_permit(id);
    if (p_ex_permit) p_ex_permit(id);
    if (r == 0) { n++; LOG("share: permit feature %d -> OK", id); }
    else if ((id % 8) == 0) LOG("share: permit feature %d -> 0x%x", id, r);
  }
  LOG("share: permitted %d of %d features", n, hi - lo + 1);
  return n;
}
