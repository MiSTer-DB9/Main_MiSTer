// [MiSTer-DB9-Pro BEGIN] - key gating (magic-bytes file at /media/fat/db9pro.key)
//
// Threat model: anti-casual-sharing, NOT crypto-grade DRM. The 32-byte magic
// is extractable from the official binary via `strings`/`objdump`. Same key
// for every user. Acceptable per locked design.
//
// Secret injection: DB9_KEY_MAGIC lives in db9_key_secret.h, which is NOT
// committed. The official CI build materializes it from GitHub secret
// DB9_KEY_MAGIC_HEX before compile. Public clones fail-closed: __has_include
// skips the include, DB9_KEY_ENABLE stays undefined, db9_key_refresh() is a
// no-op, s_unlocked remains 0 → Saturn permanently locked.
//
// File format: exactly 32 bytes at /media/fat/db9pro.key matching DB9_KEY_MAGIC.
// Re-read on each core launch via db9_key_refresh(); cached in s_unlocked.

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "db9_key.h"
#if defined __has_include
#  if __has_include ("db9_key_secret.h")
#    include "db9_key_secret.h"
#    define DB9_KEY_ENABLE
#  endif
#endif

#define DB9_KEY_PATH "/media/fat/db9pro.key"
#define DB9_KEY_LEN  32

static int s_unlocked = 0;

// Constant-time compare to avoid leaking byte position on mismatch.
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t diff = 0;
	for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
	return diff == 0;
}

void db9_key_refresh()
{
#ifdef DB9_KEY_ENABLE
	s_unlocked = 0;

	int fd = open(DB9_KEY_PATH, O_RDONLY);
	if (fd < 0) {
		printf("[DB9-Key] Saturn locked (key file missing at %s)\n", DB9_KEY_PATH);
		return;
	}

	uint8_t buf[DB9_KEY_LEN];
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n != (ssize_t)DB9_KEY_LEN) {
		printf("[DB9-Key] Saturn locked (key file wrong size: %zd, expected %d)\n",
			n, DB9_KEY_LEN);
		return;
	}

	if (!ct_equal(buf, DB9_KEY_MAGIC, DB9_KEY_LEN)) {
		printf("[DB9-Key] Saturn locked (key file invalid)\n");
		return;
	}

	s_unlocked = 1;
	printf("[DB9-Key] Saturn unlocked\n");
#endif
}

int db9_key_saturn_unlocked()
{
	return s_unlocked;
}
// [MiSTer-DB9-Pro END]
