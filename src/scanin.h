/* Copyright (C) 2026 heydemoura
 *
 * Scan-in tap enable.
 *
 * libSceAvcap2 captures a tap of the composited output, but the tap has to be
 * active on the SceSysAvControl daemon first; otherwise sceAvcap2OpenVideo is
 * rejected with 0x81950008 (no scan-in for this caller). This module turns the
 * capture tap on by talking to the daemon over IPMI (libSceIpmi), resolved at
 * runtime.
 *
 * It enables a *read tap* of the current signal. It deliberately never calls
 * sceAvControlChangeScanInMode, which would change the console's actual output
 * mode and is the call documented to be able to disturb the display engine.
 * Enabling the tap does not change what the TV shows.
 */
#ifndef PS5HS_SCANIN_H
#define PS5HS_SCANIN_H

/* Enable the capture tap for the given resolution index (11 = 1920x1080).
 * Returns 0 on success, or a negative/SCE error code. Safe to call more than
 * once; the daemon session is kept open. */
int scanin_enable(int res_index);

#endif /* PS5HS_SCANIN_H */
