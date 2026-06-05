// MiSTer-DB9 fork-only: see dtb_patcher.h for behaviour.
//
// Adapted from MiSTer-1200p (https://github.com/MiSTer-1200p/Main_MiSTer, commit
// bb42227, "Add 1920x1200 framebuffer support") with three substantive deviations:
//   1. Conditional: patch up only when the configured VIDEO_MODE* requires more
//      than the default 8 MiB reservation; patch down when it does not.
//   2. No reboot: upstream calls reboot(0) after patching. We never reboot; the
//      user picks up the change on the next manual reboot.
//   3. No .orig backup: both directions go through fdtput symmetrically. Avoids
//      the stale-backup hazard after update_all.sh overwrites zImage_dtb but
//      leaves a now-outdated zImage_dtb.orig behind.
//
// Also exposes dtb_current_pixel_cap() so the video.cpp fb_scale logic can
// clamp this session within the running kernel's reservation, surviving the
// gap between "we re-patched zImage_dtb" and "the user rebooted to load it".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "cfg.h"

#define ZIMAGE_PATH      "/media/fat/linux/zImage_dtb"
#define DTB_REG_PATH     "/proc/device-tree/MiSTer_fb/reg"
#define DTB_TMP_PATH     "/tmp/mister_fb.dtb"

#define FB_REG_BASE      0x22000000u
#define FB_SIZE_SMALL    0x00800000u   // 8 MiB
#define FB_SIZE_LARGE    0x01000000u   // 16 MiB

static int  s_cap_pixels   = 0;
static bool s_already_done = false;

static int read_dtb_reg_bytes()
{
	int fd = open(DTB_REG_PATH, O_RDONLY);
	if (fd < 0) return -1;

	uint8_t reg[8];
	ssize_t n = read(fd, reg, sizeof(reg));
	close(fd);

	if (n != (ssize_t)sizeof(reg)) return -1;

	// DTB cells are big-endian uint32; first 4 = base, next 4 = size.
	uint32_t base = ((uint32_t)reg[0] << 24) | ((uint32_t)reg[1] << 16)
	              | ((uint32_t)reg[2] <<  8) |  (uint32_t)reg[3];
	uint32_t size = ((uint32_t)reg[4] << 24) | ((uint32_t)reg[5] << 16)
	              | ((uint32_t)reg[6] <<  8) |  (uint32_t)reg[7];

	if (base != FB_REG_BASE) return -1;
	return (int)size;
}

int dtb_current_pixel_cap()
{
	if (!s_cap_pixels)
	{
		int b = read_dtb_reg_bytes();
		// Conservative fallback when /proc lookup fails: assume the unpatched
		// 8 MiB reservation so we never overshoot the kernel's view.
		if (b <= 0) b = (int)FB_SIZE_SMALL;
		s_cap_pixels = b / 4;
	}
	return s_cap_pixels;
}

// Mini-parser: returns the per-buffer byte footprint (W*H*4) implied by the
// VIDEO_MODE string, or 0 when the string is empty / malformed / a small
// builtin index. Handles the three syntactic forms `parse_custom_video_mode`
// in video.cpp accepts:
//   - Single integer N: builtin vmodes[N]. Only modes 12/13/14 exceed 8 MiB.
//   - "W,H[,refresh]"   (CVT shortcut).
//   - "W,hfp,hsync,hbp,H,..."  (9- or 11-token modeline).
//   - 21+ tokens, first slot is a flag (=1), val[1]=W, val[5]=H (full form).
static uint32_t parse_video_conf_bytes(const char *vcfg)
{
	if (!vcfg || !vcfg[0]) return 0;

	uint32_t vals[8] = {0};
	int n = 0;
	const char *p = vcfg;
	while (*p && n < (int)(sizeof(vals)/sizeof(vals[0])))
	{
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 0);
		if (!end || end == p) break;
		vals[n++] = (uint32_t)v;
		p = end;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != ',') break;
		p++;
	}

	if (n == 0) return 0;

	uint32_t W = 0, H = 0;

	if (n == 1)
	{
		// Builtin index. Mirror the resolutions hardcoded in vmodes[] for the
		// only entries that exceed 8 MiB single-buffer; everything else returns
		// 0 (no patch needed).
		switch (vals[0])
		{
			case 12: W = 1920; H = 1440; break; // 1920x1440@60
			case 13: W = 2048; H = 1536; break; // 2048x1536@60
			case 14: W = 2560; H = 1440; break; // 2560x1440@60 (pr)
			default: return 0;
		}
	}
	else if (n == 2 || n == 3)
	{
		// CVT shortcut: width, height [, refresh]
		W = vals[0];
		H = vals[1];
	}
	else
	{
		// Modeline (9/11 tokens) vs full-form (21+ tokens): tell them apart by
		// the leading flag word. The full form starts with `1` (an enable flag)
		// and stores width at val[1], height at val[5].
		if (vals[0] == 1 && vals[1] >= 320)
		{
			W = vals[1];
			H = vals[5];
		}
		else
		{
			W = vals[0];
			H = vals[4];
		}
	}

	if (W < 320 || H < 200 || W > 4096 || H > 2160) return 0; // sanity
	return W * H * 4u;
}

static uint32_t max_configured_bytes()
{
	uint32_t a = parse_video_conf_bytes(cfg.video_conf);
	uint32_t b = parse_video_conf_bytes(cfg.video_conf_pal);
	uint32_t c = parse_video_conf_bytes(cfg.video_conf_ntsc);
	uint32_t m = a;
	if (b > m) m = b;
	if (c > m) m = c;
	return m;
}

// Locate the appended DTB inside zImage_dtb. ARM zImage stores the
// uncompressed image start/end virtual addresses at file offsets 40 and 44;
// the DTB blob sits immediately after the image (offset = end - start).
static int locate_appended_dtb(FILE *f, long file_size, uint32_t *dtb_off, uint32_t *dtb_size)
{
	uint32_t img_start = 0, img_end = 0;
	if (fseek(f, 40, SEEK_SET) != 0) return -1;
	if (fread(&img_start, 4, 1, f) != 1) return -1;
	if (fseek(f, 44, SEEK_SET) != 0) return -1;
	if (fread(&img_end, 4, 1, f) != 1) return -1;

	uint32_t off = img_end - img_start;
	if (off >= (uint32_t)file_size) return -1;

	*dtb_off  = off;
	*dtb_size = (uint32_t)file_size - off;
	return 0;
}

// Write the patched DTB back into zImage at the same offset, truncating to
// match the new total length (the patched DTB may be a few bytes smaller or
// larger than the original after fdtput rewrites the property).
static int splice_dtb_into_zimage(uint32_t dtb_off, const uint8_t *patched, uint32_t patched_size)
{
	FILE *f = fopen(ZIMAGE_PATH, "r+b");
	if (!f) return -1;

	if (fseek(f, (long)dtb_off, SEEK_SET) != 0) { fclose(f); return -1; }
	if (fwrite(patched, 1, patched_size, f) != patched_size) { fclose(f); return -1; }

	fflush(f);
	int fd = fileno(f);
	if (fd >= 0) fsync(fd);
	int trc = ftruncate(fd, (long)dtb_off + (long)patched_size);
	fclose(f);
	sync();
	return trc;
}

// Re-write zImage_dtb so the appended DTB's MiSTer_fb.reg cell pair is
// `<FB_REG_BASE new_size>`. Idempotent: if the on-disk DTB already encodes
// new_size, no I/O is performed. Used in both directions (8 MiB <-> 16 MiB).
// Returns: 0 = no change needed (already at target), 1 = rewrote, -1 = error.
static int repatch_dtb_reg(uint32_t new_size)
{
	FILE *f = fopen(ZIMAGE_PATH, "rb");
	if (!f)
	{
		printf("DTB: cannot open %s for read.\n", ZIMAGE_PATH);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long file_size = ftell(f);
	if (file_size <= 64) { fclose(f); return -1; }

	uint32_t dtb_off = 0, dtb_size = 0;
	if (locate_appended_dtb(f, file_size, &dtb_off, &dtb_size) != 0)
	{
		printf("DTB: cannot locate appended DTB in %s.\n", ZIMAGE_PATH);
		fclose(f);
		return -1;
	}

	uint8_t *dtb = (uint8_t *)malloc(dtb_size);
	if (!dtb) { fclose(f); return -1; }
	if (fseek(f, (long)dtb_off, SEEK_SET) != 0
	    || fread(dtb, 1, dtb_size, f) != dtb_size)
	{
		free(dtb);
		fclose(f);
		return -1;
	}
	fclose(f);

	FILE *tmp = fopen(DTB_TMP_PATH, "wb");
	if (!tmp) { free(dtb); return -1; }
	size_t wn = fwrite(dtb, 1, dtb_size, tmp);
	fclose(tmp);
	if (wn != dtb_size) { free(dtb); unlink(DTB_TMP_PATH); return -1; }

	char cmd[256];
	snprintf(cmd, sizeof(cmd),
	         "fdtput -t x %s /MiSTer_fb reg 0x%x 0x%x",
	         DTB_TMP_PATH, FB_REG_BASE, new_size);
	int ret = system(cmd);
	if (ret == -1 || !WIFEXITED(ret) || WEXITSTATUS(ret) != 0)
	{
		printf("DTB: fdtput failed (rc=%d).\n", WIFEXITED(ret) ? WEXITSTATUS(ret) : ret);
		free(dtb);
		unlink(DTB_TMP_PATH);
		return -1;
	}

	FILE *pf = fopen(DTB_TMP_PATH, "rb");
	if (!pf) { free(dtb); unlink(DTB_TMP_PATH); return -1; }
	if (fseek(pf, 0, SEEK_END) != 0) { fclose(pf); free(dtb); unlink(DTB_TMP_PATH); return -1; }
	long patched_size = ftell(pf);
	if (patched_size <= 0) { fclose(pf); free(dtb); unlink(DTB_TMP_PATH); return -1; }
	if (fseek(pf, 0, SEEK_SET) != 0) { fclose(pf); free(dtb); unlink(DTB_TMP_PATH); return -1; }
	uint8_t *patched = (uint8_t *)malloc(patched_size);
	if (!patched) { fclose(pf); free(dtb); unlink(DTB_TMP_PATH); return -1; }
	if (fread(patched, 1, patched_size, pf) != (size_t)patched_size)
	{
		free(patched);
		fclose(pf);
		free(dtb);
		unlink(DTB_TMP_PATH);
		return -1;
	}
	fclose(pf);
	unlink(DTB_TMP_PATH);

	// Idempotence: if fdtput produced bytes identical to what we extracted,
	// the on-disk zImage is already at the target reservation -> skip splice.
	// This matters when MiSTer.ini was toggled multiple times in one session
	// (e.g. 1200p -> 1080p -> 1200p without a reboot in between): the running
	// kernel's /proc still reports the original reservation, but the on-disk
	// DTB has already been updated by an earlier reconcile pass.
	int rc;
	if ((uint32_t)patched_size == dtb_size && memcmp(patched, dtb, dtb_size) == 0)
	{
		rc = 0;
	}
	else
	{
		rc = splice_dtb_into_zimage(dtb_off, patched, (uint32_t)patched_size);
		if (rc == 0) rc = 1;
	}
	free(patched);
	free(dtb);
	return rc;
}

void dtb_reconcile_for_video_mode()
{
	if (s_already_done) return;
	s_already_done = true;

	int cap_bytes = read_dtb_reg_bytes();
	if (cap_bytes > 0) s_cap_pixels = cap_bytes / 4;

	// Larger reservation is only meaningful when the user opted into the
	// full-resolution framebuffer via fb_size=1. Any other fb_size (0=auto/half,
	// 2/3/4=fixed scale) keeps the pre-1200p behaviour, so target the small
	// reservation — this also reverts an earlier 16 MiB patch when the user
	// switches fb_size back to 0.
	uint32_t needed = (cfg.fb_size == 1) ? max_configured_bytes() : 0;
	uint32_t target_size = (needed > FB_SIZE_SMALL) ? FB_SIZE_LARGE : FB_SIZE_SMALL;

	int rc = repatch_dtb_reg(target_size);
	if (rc < 0)
	{
		printf("DTB: reconcile to %u MiB failed; running at the current reservation.\n",
		       target_size / (1024u * 1024u));
		return;
	}
	if (rc == 0)
	{
		// Disk already matches the configured mode; nothing to do.
		return;
	}
	if (target_size > FB_SIZE_SMALL)
		printf("DTB: patched MiSTer_fb reservation to 16 MiB. Reboot to apply.\n");
	else
		printf("DTB: MiSTer_fb reservation no longer needed; restored to 8 MiB. Reboot to apply.\n");
}
