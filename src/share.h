/* Copyright (C) 2026 heydemoura
 *
 * Share feature permit. libSceAvcap2's open path is gated by a Share feature
 * permit (SceShellCore calls sceShareFeaturePermit before opening the capture
 * tap). sceShareFeaturePermit records the permitted feature in a per-process
 * object that the capture path checks. We resolve it at runtime and permit the
 * capture/broadcast feature(s) before opening.
 */
#ifndef PS5HS_SHARE_H
#define PS5HS_SHARE_H

/* Permit a single Share feature id. Returns the SCE code (0 = permitted). */
int share_permit(int feature_id);

/* Permit a range of feature ids [lo, hi] and log which succeed. Returns the
 * count permitted. Used to discover the capture feature id empirically. */
int share_permit_sweep(int lo, int hi);

#endif /* PS5HS_SHARE_H */
