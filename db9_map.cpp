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
	if (!strcasecmp(n, "R") || !strcasecmp(n, "RT")) return CLS_R;   // Coin is NOT a shoulder (own category below)
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
// First free primary face-button raw bit at or above `first` (capped at 9). Used
// both to pack gameplay buttons positionally (first=4: A,B,C,X,Y,Z in J1 order, so
// a 6-button core fills A..F instead of dropping the overflow) and to spill a
// secondary/Select button onto the spare X,Y,Z faces (first=7 -- gameplay owns
// 4..6). Returns -1 when no face button at/above `first` is free. Start/Select/
// Mode/L/R (raw>=10) are never face targets -- a button with no home stays unmapped.
static int db9_next_free_face_raw(uint16_t used, int first)
{
	for (int r = first; r <= 9; r++) if (!(used & (1u << r))) return r;
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

// Convention categories (DB9MD/DB15 default layout, A/B/C-first). Each J1 label
// resolves to exactly ONE category; the factory-default passes then route the raw
// source per category. Mirrors the audited reference in derive_preview.py.
//   GAMEPLAY  - the "hardware" buttons; pack onto A,B,C,X,Y,Z (raw 4..9) in J1 order
//   START     - Start/Run -> raw10
//   COIN      - the credit button -> raw11 (Mode on DB9MD, reachable as Start+B on
//               a 3-button pad); priority over SEL for raw11
//   SEL       - Select/Mode -> raw11 if still free, else spills to a spare face
//   SHOULDER  - L/R triggers -> fixed shoulder raw (DB9MD has none)
//   SECONDARY - non-hardware meta buttons (Pause/Test/Start 2P/Coin 2 ...) -> spare X/Y/Z
//   NOMAP     - SaveState etc. -> unmapped (matches USB)
enum db9_cat { CAT_GAMEPLAY = 0, CAT_NOMAP, CAT_SHOULDER, CAT_START, CAT_COIN, CAT_SEL, CAT_SECONDARY };

static int ci_starts(const char *n, const char *p) { return !strncasecmp(n, p, strlen(p)); }
static int ci_ends_b(const char *n) { size_t l = strlen(n); return l && (n[l - 1] == 'b' || n[l - 1] == 'B'); }

static db9_cat db9_category(const char *n)
{
	static const char *const secondary[] = {
		"pause", "test", "service", "service select", "service mode", "reset",
		"soft reset", "cheat", "advance", "auto up", "high score reset", "slam",
		"next track", "dip", "tilt"
	};
	if (!strcasecmp(n, "savestate") || !strcasecmp(n, "save state")) return CAT_NOMAP;
	if (!strcasecmp(n, "l") || !strcasecmp(n, "lt") ||
	    !strcasecmp(n, "r") || !strcasecmp(n, "rt")) return CAT_SHOULDER;
	if ((ci_starts(n, "start") && !strchr(n, '2')) ||
	    !strcasecmp(n, "run") || !strcasecmp(n, "vstart")) return CAT_START;
	if (ci_starts(n, "coin") && !strchr(n, '2') && !ci_ends_b(n)) return CAT_COIN;
	if (!strcasecmp(n, "select") || !strcasecmp(n, "mode") ||
	    !strcasecmp(n, "game select")) return CAT_SEL;
	for (size_t i = 0; i < sizeof(secondary) / sizeof(*secondary); i++)
		if (!strcasecmp(n, secondary[i])) return CAT_SECONDARY;
	if (strcasestr(n, "start") && strchr(n, '2')) return CAT_SECONDARY;   // Start 2P
	if (ci_starts(n, "coin") && (strchr(n, '2') || ci_ends_b(n))) return CAT_SECONDARY; // Coin 2 / Coin B
	return CAT_GAMEPLAY;
}

void db9_map_factory_default(int devtype, uint8_t *map)
{
	// Derive the layout from the core's CONF_STR J1 button labels.
	// DB9MD/DB15/Saturn pads physically carry the core's native button names, so
	// each J1 label is mapped to the same-named physical button (A->A, C->C, X->X
	// ...) by the Pass-1 exact match below -- a native pad stays byte-identical.
	// jn/jp (the USB SNES-layout remap) are intentionally NOT consulted: they would
	// scramble a native pad (e.g. MegaDrive jn renames C->R, Mode->Select, Z->L,
	// killing C/Z). A label that matches no same-named pad button is routed by its
	// db9_category (Start/Coin/Select/shoulder/secondary) or, failing that, packed
	// positionally onto the next free face button (e.g. TG16 "Button I", arcade
	// "Shot"). Output slot = raw J1 position + 4 == this core's joystick_0 bit for
	// that button, so slot order follows the core's own J1 (dashes hold a position).
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

	// Pre-scan: does the core consume an R/RT shoulder button? Decides the Saturn
	// Select/Coin source (Start+B combo when R is busy, else the Saturn R trigger).
	// R/RT only -- Coin is its own category and never counts as an R shoulder.
	int has_R = 0;
	for (int k = 0; ; k++)
	{
		int pos;
		const char *name = db9_slot_name(k, &pos);
		if (!name || pos + 4 > DB9_MAP_BTN_LAST) break;
		if (!strcasecmp(name, "R") || !strcasecmp(name, "RT")) { has_R = 1; break; }
	}

	// Pass 1: exact label->pad-button matches (same-family: lossless) claim their
	// raw bits first, so a console native pad stays byte-identical (Genesis "C"
	// exact->raw6, "Z"->raw9, "Start"->raw10, "Mode"->raw11 ...) and a later
	// category pass can never double-book a button the core names outright.
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

	// Helper: claim raw11 (Mode/Select; Start+B combo on Saturn when R is busy).
	// Returns 1 if the slot was placed. Combos don't reserve a raw bit.
	auto claim_sel = [&](int slot) -> int {
		if (devtype == DB9_DEV_SATURN && has_R) { map[slot] = DB9_MAP_COMBO_STARTB; return 1; }
		if (!(used & (1u << 11))) { map[slot] = 11; used |= 1u << 11; return 1; }
		return 0;
	};

	// Visit every still-unmapped real J1 button once, handing (slot, name, category)
	// to `place`. Invoked once per category below; map[slot]!=NONE short-circuits a
	// button already claimed by an earlier pass.
	auto each = [&](auto place) {
		for (int k = 0; ; k++)
		{
			int pos;
			const char *name = db9_slot_name(k, &pos);
			if (!name) break;
			int slot = pos + 4;
			if (slot > DB9_MAP_BTN_LAST) break;
			if (map[slot] != DB9_MAP_NONE) continue;
			place(slot, name, db9_category(name));
		}
	};

	// Category passes, in the order that fixes the raw budget:
	//   START(10) -> COIN(11) -> SEL(11 if free) -> SHOULDER -> GAMEPLAY(4..9)
	//   -> SECONDARY (+ COIN/SEL that lost the r11 race) on spare faces 7..9.
	// Gameplay runs before secondary so the "hardware" buttons claim A,B,C first
	// and a <=3-button core stays 3-button-playable. Coin beats Select for r11.
	each([&](int slot, const char *, db9_cat cat) {
		if (cat == CAT_START && !(used & (1u << 10))) { map[slot] = 10; used |= 1u << 10; }
	});
	each([&](int slot, const char *, db9_cat cat) { if (cat == CAT_COIN) claim_sel(slot); });
	each([&](int slot, const char *, db9_cat cat) { if (cat == CAT_SEL)  claim_sel(slot); });
	each([&](int slot, const char *name, db9_cat cat) {
		if (cat == CAT_SHOULDER)
		{
			int raw = db9_class_raw(devtype, db9_classify(name), has_R);
			if (raw != DB9_MAP_NONE && !(used & (1u << raw))) { map[slot] = (uint8_t)raw; used |= 1u << raw; }
		}
	});
	each([&](int slot, const char *, db9_cat cat) {
		if (cat == CAT_GAMEPLAY)
		{
			int raw = db9_next_free_face_raw(used, 4);   // A,B,C,X,Y,Z (4..9) in J1 order
			if (raw >= 0) { map[slot] = (uint8_t)raw; used |= 1u << raw; }
		}
	});
	each([&](int slot, const char *, db9_cat cat) {
		if (cat == CAT_SECONDARY || cat == CAT_SEL || cat == CAT_COIN)
		{
			int raw = db9_next_free_face_raw(used, 7);    // spare X,Y,Z faces -- gameplay owns 4..6
			if (raw >= 0) { map[slot] = (uint8_t)raw; used |= 1u << raw; }
		}
	});

	// Anything still unmapped (SaveState, an overflow keypad/peripheral button on a
	// >6-button core, Select with no spare face) stays unmapped -- same as USB,
	// where map_joystick leaves unresolved J1 entries unbound until Define.
}
// [MiSTer-DB9 END]
