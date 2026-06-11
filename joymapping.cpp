/*
This file contains lookup information on known controllers
*/

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "joymapping.h"
#include "menu.h"
#include "input.h"
#include "user_io.h"
#include "cfg.h"

#define DPAD_COUNT 4

/*****************************************************************************/
static void trim(char * s)
{
	char *p = s;
	int l = strlen(p);
	if (!l) return;

	while (p[l - 1] == ' ') p[--l] = 0;
	while (*p && (*p == ' ')) ++p, --l;

	memmove(s, p, l + 1);
}

static char joy_names[NUMBUTTONS][32];
static int joy_count = 0;

static char joy_nnames[NUMBUTTONS][32];
static char joy_pnames[NUMBUTTONS][32];
static int defaults = 0;

static void read_buttons()
{
	char *p;

	memset(joy_names, 0, sizeof(joy_names));
	memset(joy_nnames, 0, sizeof(joy_nnames));
	memset(joy_pnames, 0, sizeof(joy_pnames));
	joy_count = 0;
	defaults = 0;

	user_io_read_confstr();

	// this option used as default name map (unless jn/jp is supplied)
	p = get_buttons(0);
	if (p)
	{
		for (int n = 0; n < NUMBUTTONS - DPAD_COUNT; n++)
		{
			substrcpy(joy_names[n], p, n);
			if (!joy_names[n][0]) break;

			printf("joy_name[%d] = %s\n", n, joy_names[n]);

			memcpy(joy_nnames[n], joy_names[n], sizeof(joy_nnames[0]));
			char *sstr = strchr(joy_nnames[n], '(');
			if (sstr) *sstr = 0;
			trim(joy_nnames[n]);

			if (!joy_nnames[n][0]) break;
			joy_count++;
		}
		printf("\n");
	}

	// - supports empty name to skip the button from default map
	// - only base button names must be used (ABXYLR Start Select)

	// name default map
	p = get_buttons(1);
	if (p)
	{
		memset(joy_nnames, 0, sizeof(joy_nnames));
		for (int n = 0; n < joy_count; n++)
		{
			substrcpy(joy_nnames[n], p, n);
			trim(joy_nnames[n]);
			if (joy_nnames[n][0]) printf("joy_nname[%d] = %s\n", n, joy_nnames[n]);
		}
		printf("\n");
	}

	// positional default map
	p = get_buttons(2);
	if (p)
	{
		defaults = cfg.gamepad_defaults;
		for (int n = 0; n < joy_count; n++)
		{
			substrcpy(joy_pnames[n], p, n);
			trim(joy_pnames[n]);
			if (joy_pnames[n][0]) printf("joy_pname[%d] = %s\n", n, joy_pnames[n]);
		}
		printf("\n");
	}
}

static int is_fire(char* name)
{
	if (!strncasecmp(name, "fire", 4) || !strncasecmp(name, "button", 6))
	{
		if (!strcasecmp(name, "fire") || strchr(name, '1')) return 1;
		if (strchr(name, '2')) return 2;
		if (strchr(name, '3')) return 3;
		if (strchr(name, '4')) return 4;
	}

	return 0;
}

void map_joystick(uint32_t *map, uint32_t *mmap)
{
	/*
	attemps to centrally defined core joy mapping to the joystick declaredy by a core config string
	we use the names declared by core with some special handling for specific edge cases

	Input button order is "virtual SNES" i.e.:
		A, B, X, Y, L, R, Select, Start
	*/
	read_buttons();

	map[SYS_BTN_RIGHT] = mmap[SYS_BTN_RIGHT] & 0xFFFF;
	map[SYS_BTN_LEFT]  = mmap[SYS_BTN_LEFT]  & 0xFFFF;
	map[SYS_BTN_DOWN]  = mmap[SYS_BTN_DOWN]  & 0xFFFF;
	map[SYS_BTN_UP]    = mmap[SYS_BTN_UP]    & 0xFFFF;

	if (mmap[SYS_AXIS_X] && !is_psx())
	{
		uint32_t key = KEY_EMU + (((uint16_t)mmap[SYS_AXIS_X]) << 1);
		map[SYS_BTN_LEFT] = (key << 16) | map[SYS_BTN_LEFT];
		map[SYS_BTN_RIGHT] = ((key+1) << 16) | map[SYS_BTN_RIGHT];
	}

	if (mmap[SYS_AXIS_Y] && !is_psx())
	{
		uint32_t key = KEY_EMU + (((uint16_t)mmap[SYS_AXIS_Y]) << 1);
		map[SYS_BTN_UP] = (key << 16) | map[SYS_BTN_UP];
		map[SYS_BTN_DOWN] = ((key + 1) << 16) | map[SYS_BTN_DOWN];
	}

	// loop through core requested buttons and construct result map
	for (int i=0, n=0; i<joy_count; i++)
	{
		if (!strcmp(joy_names[i], "-")) continue;

		int idx = i+DPAD_COUNT;
		char btn_name[32];
		strcpy(btn_name, defaults ? joy_pnames[n] : joy_nnames[n]);

		char *p = strchr(btn_name, '|');
		if (p) *p = 0;

		if(!strcasecmp(btn_name, "A")
		|| !strcasecmp(btn_name, "Jump")
		|| is_fire(btn_name) == 1)
		{
			map[idx] = mmap[SYS_BTN_A];
		}

		else if(!strcasecmp(btn_name, "B")
		|| is_fire(btn_name) == 2)
		{
			map[idx] = mmap[SYS_BTN_B];
		}

		else if(!strcasecmp(btn_name, "X")
		|| !strcasecmp(btn_name, "C")
		|| is_fire(btn_name) == 3)
		{
			map[idx] = mmap[SYS_BTN_X];
		}

		else if(!strcasecmp(btn_name, "Y")
		|| !strcasecmp(btn_name, "D")
		|| is_fire(btn_name) == 4)
		{
			map[idx] = mmap[SYS_BTN_Y];
		}

		// Genesis C and Z  and TG16 V and VI
		else if(!strcasecmp(btn_name, "R")
		|| !strcasecmp(btn_name, "RT")
		|| !strcasecmp(btn_name, "Coin"))
		{
			map[idx] = mmap[SYS_BTN_R];
		}

		else if(!strcasecmp(btn_name, "L")
		|| !strcasecmp(btn_name, "LT"))
		{
			map[idx] = mmap[SYS_BTN_L];
		}

		else if(!strcasecmp(btn_name, "Select")
		|| !strcasecmp(btn_name, "Mode")
		|| !strcasecmp(btn_name, "Game Select")
		|| !strcasecmp(btn_name, "Start 2P"))
		{
			map[idx] = mmap[SYS_BTN_SELECT];
		}

		else if(!strcasecmp(btn_name, "Start")
		|| !strcasecmp(btn_name, "Run")
		|| !strcasecmp(btn_name, "Pause")
		|| !strcasecmp(btn_name, "Start 1P"))
		{
			map[idx] = mmap[SYS_BTN_START];
		}

		n++;
	}
}

int map_paddle_btn()
{
	read_buttons();
	for (int i = 0, n = 0; i < joy_count; i++)
	{
		if (!strcmp(joy_names[i], "-")) continue;
		char *p = strchr(defaults ? joy_pnames[n] : joy_nnames[n], '|');
		if (p && !strcasecmp(p, "|P")) return i + SYS_BTN_A;
		n++;
	}
	return SYS_BTN_A;
}

static const char* get_std_name(uint16_t code, uint32_t *mmap)
{
	if (code)
	{
		if (code == mmap[SYS_BTN_A]) return "[A]";
		if (code == mmap[SYS_BTN_B]) return "[B]";
		if (code == mmap[SYS_BTN_X]) return "[X]";
		if (code == mmap[SYS_BTN_Y]) return "[Y]";
		if (code == mmap[SYS_BTN_L]) return "[L]";
		if (code == mmap[SYS_BTN_R]) return "[R]";
		if (code == mmap[SYS_BTN_SELECT]) return "[\x96]";
		if (code == mmap[SYS_BTN_START]) return "[\x16]";
		return "[ ]";
	}
	return NULL;
}

void map_joystick_show(uint32_t *map, uint32_t *mmap, int num)
{
	static char mapinfo[1024];
	read_buttons();

	sprintf(mapinfo, "Map (P%d):", num);
	if (!num) sprintf(mapinfo, " Map:");
	char *list = mapinfo + strlen(mapinfo);

	// loop through core requested buttons and construct result map
	for (int i = 0; i < joy_count; i++)
	{
		if (!strcmp(joy_names[i], "-")) continue;

		const char *btn = get_std_name((uint16_t)(map[i + DPAD_COUNT]), mmap);
		if (btn)
		{
			strcat(mapinfo, "\n");
			strcat(mapinfo, btn);
			strcat(mapinfo, ": ");
			strcat(mapinfo, joy_names[i]);
		}
	}

	if(strlen(list) && cfg.controller_info) Info(mapinfo, cfg.controller_info * 1000);
}

// [MiSTer-DB9 BEGIN] - accessors for the DB9 factory-default derive (db9_map.cpp).
// Reuse the existing read_buttons() static state (J1/jn/jp + cfg.gamepad_defaults)
// so the DB9 layout follows the same per-core button declarations as USB.
void db9_read_default_names()
{
	read_buttons();
}

int db9_default_name_count()
{
	return joy_count;
}

// Truncate a CONF_STR button label to its bare name into the caller's buffer:
// '|' marker ("R|P" -> "R") and '(' annotation ("A (turbo)" -> "A") cut, then
// trimmed.
static const char *db9_clean_label(char *dst, int dstsz, const char *label)
{
	strncpy(dst, label, dstsz - 1);
	dst[dstsz - 1] = 0;
	char *p = strchr(dst, '|'); if (p) *p = 0;
	p = strchr(dst, '(');       if (p) *p = 0;
	trim(dst);
	return dst;
}

// Yields the k-th real (non-"-") J1 button for the DB9 derive: returns the core's
// OWN J1 button label (cleaned via db9_clean_label) and sets *out_pos to its raw
// J1 position. DB9MD/DB15/Saturn pads physically carry the core's native button
// names (Genesis A,B,C,X,Y,Z; Saturn A,B,C,X,Y,Z,L,R; ...), so the derive maps
// each label to the same-named physical button. NOT the jn/jp SNES-pad remap:
// jn/jp rename buttons for a USB SNES-layout pad (e.g. MegaDrive jn maps C->"R",
// Mode->"Select", Z->"L"), which scrambles a native DB9MD/Saturn pad and leaves
// its C/Z dead. Returns NULL once k is past the last real button; dash-skip
// indexing preserved so *out_pos == map_joystick's idx-DPAD_COUNT.
const char *db9_slot_name(int k, int *out_pos)
{
	static char name[32];
	int n = 0;
	for (int i = 0; i < joy_count; i++)
	{
		if (!strcmp(joy_names[i], "-")) continue; // "-" placeholder: doesn't advance n
		if (n == k)
		{
			if (out_pos) *out_pos = i;
			return db9_clean_label(name, sizeof(name), joy_names[i]); // core's own J1 label
		}
		n++;
	}
	return NULL;
}
// [MiSTer-DB9 END]
