#ifndef DTB_PATCHER_H
#define DTB_PATCHER_H

// MiSTer-DB9 fork-only: reconcile the kernel MiSTer_fb DTB reservation on
// /media/fat/linux/zImage_dtb with the framebuffer footprint required by the
// configured VIDEO_MODE / VIDEO_MODE_PAL / VIDEO_MODE_NTSC strings.
//
// Patches up (8 MiB -> 16 MiB) only when the configured mode exceeds 8 MiB
// per buffer (1920x1200 and beyond). Patches down (16 MiB -> 8 MiB) when no
// configured mode needs the larger reservation. The on-disk patch only takes
// effect on the next user-initiated reboot; this function never reboots.
//
// Safe to call multiple times: idempotent once disk state agrees with the
// configured mode.
void dtb_reconcile_for_video_mode();

// Per-buffer pixel cap the running kernel will accept, derived from
// /proc/device-tree/MiSTer_fb/reg at startup. Equals reservation_bytes / 4
// (e.g. 1920*1080 ~= 2097152 when DTB reserves 8 MiB; 1920*1200 fits in
// 16 MiB). fb_scale logic must clamp to this value during the session that
// follows a kernel update (when zImage_dtb on disk has been replaced and we
// have just re-patched it but the user has not yet rebooted). Returns the
// MiSTer_fb pixel cap, or a conservative 1920*1080 fallback when reading
// the DTB fails.
int dtb_current_pixel_cap();

#endif
