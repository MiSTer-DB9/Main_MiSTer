// [MiSTer-DB9 BEGIN] - programmable per-core button-remap matrix (UIO_DB9_MAP 0xFD)
// See db9_map.h for the FPGA contract. Pure packing + SPI streaming + factory
// defaults; the per-core/per-devtype .map file I/O and the "Define DB9 buttons"
// OSD flow live in input.cpp / menu.cpp.

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "db9_map.h"
#include "joymapping.h"
#include "spi.h"
#include "user_io.h"

// Set the 4-bit selector at bit offset `base` within the selector buffer.
static void set_sel(uint8_t *buf, int base, uint8_t val4)
{
	// buf is zeroed by the caller (db9_map_stream memset) and each 4-bit region
	// is written exactly once, so only the set bits need touching -- the 0-bit
	// clear arm would only re-clear already-zero bits.
	for (int i = 0; i < 4; i++)
	{
		int b = base + i;
		if (val4 & (1 << i)) buf[b >> 3] |= (uint8_t)(1 << (b & 7));
	}
}

void db9_map_stream(const uint8_t *map)
{
	// 36 bits = 9 button slots (4..12) x 4 bits, ONE shared table for both player
	// ports (the FPGA hardwires the D-pad and shares the selector across ports).
	// Streamed as 3 x 16-bit words (the low 36 bits used; buf padded to 6 bytes).
	// Byte/word order matches the FPGA's indexed-slice load
	// (sel[(word-1)*16 +: 16] <= io_din), the same little-endian-per-16-bit-word
	// convention db9_key.cpp relies on.
	uint8_t buf[6];
	memset(buf, 0, sizeof(buf));

	for (int s = DB9_MAP_BTN_FIRST; s <= DB9_MAP_BTN_LAST; s++)
	{
		uint8_t v = (uint8_t)(map[s] & 0x0F);
		set_sel(buf, (s - DB9_MAP_BTN_FIRST) * 4, v);
	}

	spi_uio_cmd_cont(UIO_DB9_MAP);
	spi_write(buf, sizeof(buf), 1);    // wide=1 -> 16-bit transfers
	DisableIO();
}

// Hardcoded per-devtype fallback, used only when the core declares no CONF_STR
// J1 button list (nothing to derive from). SNES-shaped best-effort layout.
static void db9_map_hardcoded_default(int devtype, uint8_t *map)
{
	// Common: D-pad straight through (raw 0..3 = R,L,D,U).
	map[0] = 0; map[1] = 1; map[2] = 2; map[3] = 3;

	for (int s = 4; s < DB9_MAP_SLOTS; s++) map[s] = DB9_MAP_NONE;

	switch (devtype)
	{
	case DB9_DEV_DB9MD:
		map[4] = 4;  map[5] = 5;  map[6] = 6;   // A B C
		map[7] = 7;  map[8] = 8;  map[9] = 9;   // X Y Z
		map[10] = 11;                           // Select <- Mode
		map[11] = 10;                           // Start
		break;

	case DB9_DEV_DB15:
		map[4] = 4;  map[5] = 5;  map[6] = 6;   // A B C
		map[7] = 7;  map[8] = 8;  map[9] = 9;   // D E F
		map[10] = 11;                           // Select
		map[11] = 10;                           // Start
		break;

	case DB9_DEV_SATURN:
		map[4] = 4;                             // A     -> Btn0 (A)
		map[5] = 5;                             // B     -> Btn1 (B)
		map[6] = 7;                             // X     -> Btn2 (X slot)
		map[7] = 8;                             // Y     -> Btn3 (Y slot)
		map[8] = 12;                            // L trg -> Btn4 (L slot)
		map[9] = 11;                            // R trg -> Btn5 (R slot)
		map[10] = DB9_MAP_NONE;                 // Saturn pad has no Select
		map[11] = 10;                           // Start
		break;

	default:
		break;
	}
}

// Physical DB9 button NAME -> raw bit index, per device type. The names match
// the CONF_STR J1/jn/jp tokens a core declares for the equivalent USB button,
// so an exact match routes same-family pads losslessly (Genesis pad on a
// Genesis core, Saturn pad on a Saturn core, etc.). Raw layout per db9_map.h.
struct db9_phys { const char *name; uint8_t raw; };

static const db9_phys phys_db9md[]  = { {"A",4},{"B",5},{"C",6},{"X",7},{"Y",8},{"Z",9},{"Start",10},{"Mode",11} };
static const db9_phys phys_db15[]   = { {"A",4},{"B",5},{"C",6},{"D",7},{"E",8},{"F",9},{"Start",10},{"Select",11} };
static const db9_phys phys_saturn[] = { {"A",4},{"B",5},{"C",6},{"X",7},{"Y",8},{"Z",9},{"Start",10},{"R",11},{"L",12} };

// Button classes for cross-pad aliasing. Mirrors joymapping.cpp's name ladder,
// kept as a fork-local copy on purpose -- joymapping.cpp is upstream-origin and
// must not be restructured to share it (Critical Rule #1).
enum db9_class { CLS_NONE = 0, CLS_A, CLS_B, CLS_X, CLS_Y, CLS_L, CLS_R, CLS_SEL, CLS_START };

static int fire_num(const char *n)
{
	if (strncasecmp(n, "fire", 4) && strncasecmp(n, "button", 6)) return 0;
	if (!strcasecmp(n, "fire") || strchr(n, '1')) return 1;
	if (strchr(n, '2')) return 2;
	if (strchr(n, '3')) return 3;
	if (strchr(n, '4')) return 4;
	return 0;
}

static db9_class db9_classify(const char *n)
{
	int f = fire_num(n);
	if (!strcasecmp(n, "A") || !strcasecmp(n, "Jump") || f == 1) return CLS_A;
	if (!strcasecmp(n, "B") || f == 2) return CLS_B;
	if (!strcasecmp(n, "X") || !strcasecmp(n, "C") || f == 3) return CLS_X;
	if (!strcasecmp(n, "Y") || !strcasecmp(n, "D") || f == 4) return CLS_Y;
	if (!strcasecmp(n, "R") || !strcasecmp(n, "RT") || !strcasecmp(n, "Coin")) return CLS_R;
	if (!strcasecmp(n, "L") || !strcasecmp(n, "LT")) return CLS_L;
	if (!strcasecmp(n, "Select") || !strcasecmp(n, "Mode") || !strcasecmp(n, "Game Select") || !strcasecmp(n, "Start 2P")) return CLS_SEL;
	if (!strcasecmp(n, "Start") || !strcasecmp(n, "Run") || !strcasecmp(n, "Pause") || !strcasecmp(n, "Start 1P")) return CLS_START;
	return CLS_NONE;
}

static int db9_exact_raw(int devtype, const char *name)
{
	const db9_phys *t; int n;
	switch (devtype)
	{
	case DB9_DEV_DB9MD:  t = phys_db9md;  n = sizeof(phys_db9md)  / sizeof(*t); break;
	case DB9_DEV_DB15:   t = phys_db15;   n = sizeof(phys_db15)   / sizeof(*t); break;
	case DB9_DEV_SATURN: t = phys_saturn; n = sizeof(phys_saturn) / sizeof(*t); break;
	default: return -1;
	}
	for (int i = 0; i < n; i++) if (!strcasecmp(name, t[i].name)) return t[i].raw;
	return -1;
}

// Map a cross-pad button class to this device's canonical raw bit. DB9MD has no
// L/R (no shoulders); Saturn has no Select, so its Select source is contextual:
// when the core uses R as R, Select rides a Start+B combo, otherwise it takes
// the otherwise-free Saturn R trigger (raw11).
// When a class alias's canonical pad button is already claimed (e.g. Genesis "X"
// wants DB15 raw6 but literal "C" took it), spill onto the next free primary face
// button (raw 4..9 -- a real button on every pad). Positional fallback so a
// 6-button core on a 6-button pad fills A,B,C,D,E,F in order instead of dropping
// the overflow. Returns -1 when all six face buttons are taken. Start/Select/Mode/
// L/R (raw>=10) are never spill targets -- a class with no home stays unmapped.
static int db9_next_free_face_raw(uint16_t used)
{
	for (int r = 4; r <= 9; r++) if (!(used & (1u << r))) return r;
	return -1;
}

static int db9_class_raw(int devtype, db9_class c, int has_R)
{
	switch (devtype)
	{
	case DB9_DEV_DB9MD:
		switch (c) { case CLS_A: return 4; case CLS_B: return 5; case CLS_X: return 7;
			case CLS_Y: return 8; case CLS_SEL: return 11; case CLS_START: return 10;
			default: return DB9_MAP_NONE; }
	case DB9_DEV_DB15:
		switch (c) { case CLS_A: return 4; case CLS_B: return 5; case CLS_X: return 6;
			case CLS_Y: return 7; case CLS_L: return 8; case CLS_R: return 9;
			case CLS_SEL: return 11; case CLS_START: return 10; default: return DB9_MAP_NONE; }
	case DB9_DEV_SATURN:
		switch (c) { case CLS_A: return 4; case CLS_B: return 5; case CLS_X: return 7;
			case CLS_Y: return 8; case CLS_L: return 12; case CLS_R: return 11;
			case CLS_SEL: return has_R ? DB9_MAP_COMBO_STARTB : 11;
			case CLS_START: return 10; default: return DB9_MAP_NONE; }
	}
	return DB9_MAP_NONE;
}

void db9_map_factory_default(int devtype, uint8_t *map)
{
	// Derive the layout from the core's CONF_STR J1 button labels.
	// DB9MD/DB15/Saturn pads physically carry the core's native button names, so
	// each J1 label is mapped to the same-named physical button (A->A, C->C, X->X
	// ...). jn/jp are the USB SNES-layout remap and would scramble a native pad
	// (e.g. MegaDrive jn renames C->R, Mode->Select, Z->L, killing C/Z), so they
	// are consulted ONLY for labels that resolve to nothing on their own (no
	// same-named pad button, no class -- e.g. TG16 "Button I", arcade "Shot").
	// Output slot = raw J1 position + 4 == this core's joystick_0 bit for that
	// button, so slot order follows the core's own J1 (dashes hold a position).
	db9_read_default_names();
	int nb = db9_default_name_count();
	if (nb <= 0 || !db9_slot_name(0, NULL))
	{
		// No J1, or a J1 with no real (non-"-") button -> nothing to derive
		// from; use the legacy table.
		db9_map_hardcoded_default(devtype, map);
		return;
	}

	// D-pad straight through; everything else unmapped until resolved below.
	map[0] = 0; map[1] = 1; map[2] = 2; map[3] = 3;
	for (int s = 4; s < DB9_MAP_SLOTS; s++) map[s] = DB9_MAP_NONE;

	// Pre-scan: does the core consume an R-class button? Decides the Saturn
	// Select source (Start+B combo when R is busy, else the Saturn R trigger).
	// Same effective-name rule and slot bound as the passes below, so the
	// decision matches what actually gets mapped.
	int has_R = 0;
	for (int k = 0; ; k++)
	{
		int pos;
		const char *name = db9_slot_name(k, &pos);
		if (!name || pos + 4 > DB9_MAP_BTN_LAST) break;
		db9_class c = db9_classify(name);
		if (c == CLS_NONE && db9_exact_raw(devtype, name) < 0)
			c = db9_classify(db9_slot_jn_name(k));
		if (c == CLS_R) { has_R = 1; break; }
	}

	// Pass 1: exact label->pad-button matches (same-family: lossless) claim
	// their raw bits first, so a class alias resolved in pass 2 can never
	// double-book a button the core names outright (e.g. Genesis J1 on a DB15
	// pad: "C" exact->raw6 must keep "X" CLS_X->raw6 from also firing on it).
	uint16_t used = 0;   // bitmask of claimed raw bits (raw <= 13)
	for (int k = 0; ; k++)
	{
		int pos;
		const char *name = db9_slot_name(k, &pos);
		if (!name) break;
		int slot = pos + 4;
		if (slot > DB9_MAP_BTN_LAST) break;   // 13..15 exist in map[] but never stream

		int raw = db9_exact_raw(devtype, name);
		if (raw < 0 || (used & (1u << raw))) continue;
		map[slot] = (uint8_t)raw;
		used |= 1u << raw;
	}

	// Pass 2: cross-family aliasing by class for everything still unmapped. The
	// J1 label classifies first; a label with no class falls back to the core's
	// jn/jp (SNES-canonical) name so e.g. "Button II" still lands on B. A class
	// alias never claims an already-taken raw bit (first claimant wins).
	for (int k = 0; ; k++)
	{
		int pos;
		const char *name = db9_slot_name(k, &pos);
		if (!name) break;
		int slot = pos + 4;
		if (slot > DB9_MAP_BTN_LAST) break;
		if (map[slot] != DB9_MAP_NONE) continue;

		db9_class c = db9_classify(name);
		if (c == CLS_NONE) c = db9_classify(db9_slot_jn_name(k));
		if (c == CLS_NONE) continue;
		int raw = db9_class_raw(devtype, c, has_R);
		if (raw == DB9_MAP_NONE) continue;
		if (raw < DB9_MAP_COMBO_STARTB)           // raw bit (combos aren't exclusive)
		{
			if (used & (1u << raw))               // canonical button taken by an
				raw = db9_next_free_face_raw(used); // exact/earlier alias -> spill
			if (raw < 0 || (used & (1u << raw))) continue;
			used |= 1u << raw;
		}
		map[slot] = (uint8_t)raw;
	}

	// Anything the derive didn't resolve (e.g. SaveState, which isn't a real
	// button name) stays unmapped -- same as USB, where map_joystick leaves
	// unrecognized J1 entries unbound until the user assigns them in Define.
}
// [MiSTer-DB9 END]
