// [MiSTer-DB9-Pro BEGIN] - key gating v1.5 (per-customer SipHash MAC)
//
// Threat model: anti-casual-sharing on per-customer keys with time expiry.
// Source-rebuild attacks are unblockable (GPLv2 fork) — the crypto only
// raises the bar for binary-patching attacks.
//
// File: /media/fat/db9pro.key, exactly 64 bytes:
//
//   off  size  field
//     0    4   magic = "DB9K" (0x4B394244 LE)
//     4    2   version = 0x0001
//     6    2   flags
//     8    8   customer_id (u64, non-zero)
//    16    4   issue_unix
//    20    4   expiry_unix
//    24    4   feature_mask (bit0=Saturn, bits 1..31 reserved)
//    28    4   reserved (must be 0)                       <-- signed range [0..32)
//    32    8   per_customer_seed (informational only in v1.5)
//    40    8   auth_tag = SipHash-2-4(MASTER_ROOT, bytes[0..32])
//    48   16   padding (zero)
//    64       (end)
//
// Secret injection: MASTER_ROOT lives in db9_key_secret.h, which is NOT
// committed. The official CI build materializes it from GitHub secret
// MASTER_ROOT_HEX (64 hex chars) before compile. Public clones fail-closed:
// __has_include skips the include, DB9_KEY_ENABLE stays undefined,
// db9_key_refresh() is a no-op, s_unlocked stays 0 -> Saturn permanently
// locked.
//
// FPGA contract: on success, stream 40 bytes (signed payload || auth_tag)
// to hps_io via UIO_DB9_KEY (0xFE). The FPGA's db9_key_gate.sv recomputes
// SipHash with its own MASTER_ROOT parameter and, on match, latches the
// 32-bit feature_mask whose bits drive saturn_unlocked (bit 0) etc.

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "db9_key.h"
#include "siphash24.h"
#include "spi.h"
#include "user_io.h"
#if defined __has_include
#  if __has_include ("db9_key_secret.h")
#    include "db9_key_secret.h"
#    define DB9_KEY_ENABLE
#  endif
#endif

#define DB9_KEY_PATH    "/media/fat/db9pro.key"
#define DB9_KEY_SIZE    64
#define DB9_KEY_MAGIC   0x4B394244U  // "DB9K"
#define DB9_KEY_VERSION 0x0001
#define DB9_FEATURE_SATURN 0x00000001U

#pragma pack(push, 1)
typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint64_t customer_id;
	uint32_t issue_unix;
	uint32_t expiry_unix;
	uint32_t feature_mask;
	uint32_t reserved;
	uint8_t  per_customer_seed[8];
	uint8_t  auth_tag[8];
	uint8_t  pad[16];
} db9_key_v15_t;
#pragma pack(pop)

static_assert(sizeof(db9_key_v15_t) == DB9_KEY_SIZE, "db9_key_v15_t must be 64 bytes");

// Single source of truth for unlock state: bit i set <=> feature i unlocked.
// Also conveys "any feature unlocked" via non-zero — no separate s_unlocked flag.
static uint32_t s_features = 0;

#ifdef DB9_KEY_ENABLE
// Constant-time compare. `volatile` defeats compiler optimisation back to a
// short-circuit memcmp (some GCC versions vectorise the OR-fold).
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	volatile uint8_t diff = 0;
	for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
	return diff == 0;
}

// Stream 32B signed payload || 8B auth_tag to FPGA via 0xFE.
// FPGA latches into payload + tag_in registers, kicks off SipHash, compares.
static void send_to_fpga(const db9_key_v15_t *k)
{
	uint8_t buf[40];
	memcpy(buf,      k,           32);
	memcpy(buf + 32, k->auth_tag,  8);

	spi_uio_cmd_cont(UIO_DB9_KEY);
	spi_write(buf, sizeof(buf), 1);   // wide=1 → 16-bit transfers
	DisableIO();
}
#endif

void db9_key_refresh(void)
{
#ifdef DB9_KEY_ENABLE
	s_features = 0;

	int fd = open(DB9_KEY_PATH, O_RDONLY);
	if (fd < 0) {
		printf("[DB9-Key v1.5] locked: key file missing at %s\n", DB9_KEY_PATH);
		return;
	}

	db9_key_v15_t k;
	ssize_t n = read(fd, &k, sizeof(k));
	close(fd);

	if (n != (ssize_t)DB9_KEY_SIZE) {
		printf("[DB9-Key v1.5] locked: bad size %zd (expected %d)\n", n, DB9_KEY_SIZE);
		return;
	}
	if (k.magic != DB9_KEY_MAGIC) {
		printf("[DB9-Key v1.5] locked: bad magic 0x%08x\n", k.magic);
		return;
	}
	if (k.version != DB9_KEY_VERSION) {
		printf("[DB9-Key v1.5] locked: unsupported version 0x%04x\n", k.version);
		return;
	}
	if (k.customer_id == 0) {
		printf("[DB9-Key v1.5] locked: customer_id is zero\n");
		return;
	}

	uint8_t expected[8];
	siphash24((const uint8_t*)&k, 32, MASTER_ROOT, expected);
	if (!ct_equal(expected, k.auth_tag, 8)) {
		printf("[DB9-Key v1.5] locked: bad MAC (key not signed by official MASTER_ROOT)\n");
		return;
	}

	time_t now = time(NULL);
	if ((time_t)k.expiry_unix < now) {
		printf("[DB9-Key v1.5] locked: key expired (expiry=%u, now=%lld)\n",
		       k.expiry_unix, (long long)now);
		return;
	}

	send_to_fpga(&k);

	s_features = k.feature_mask;
	printf("[DB9-Key v1.5] unlocked: customer=%llu expires=%u features=0x%08x\n",
	       (unsigned long long)k.customer_id, k.expiry_unix, k.feature_mask);
#else
	(void)s_features;
#endif
}

int db9_key_saturn_unlocked(void) { return !!(s_features & DB9_FEATURE_SATURN); }
// [MiSTer-DB9-Pro END]
