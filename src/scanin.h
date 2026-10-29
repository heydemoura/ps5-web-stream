/* Copyright (C) 2026 heydemoura
 *
 * Scan-in tap enable.
 *
 * Optional diagnostic setup of the composited-output tap through the
 * SceSysAvControl daemon's IPMI interface, resolved at runtime. A successful
 * setup does not establish an Avcap video session: OpenVideo has still returned
 * 0x81950008 after this operation. See docs/AVCAP2.md.
 *
 * It enables a *read tap* of the current signal. It deliberately never calls
 * sceAvControlChangeScanInMode, which would change the console's actual output
 * mode. This firmware-specific operation is disabled unless requested.
 */
#ifndef PS5HS_SCANIN_H
#define PS5HS_SCANIN_H

/* Enable the capture tap for the given resolution index (11 = 1920x1080).
 * Returns 0 on success, or a negative/SCE error code. The daemon session is
 * kept open; repeating setup may report that the tap already exists. */
int scanin_enable(int res_index);

#endif /* PS5HS_SCANIN_H */
