// [MiSTer-DB9 BEGIN] - programmable per-core button-remap matrix (UIO_DB9_MAP 0xFD)
//
// Companion of Forks_MiSTer/fork_ci_template/sys/joydb_remap.sv. For the active
// core + DB9 device type, Main_MiSTer streams a 36-bit selector table (3x
// 16-bit words) telling the FPGA which physical DB9/DB15/Saturn source bit
// drives each remappable joystick output slot. D-pad (slots 0..3) is hardwired
// identity in the FPGA and never streamed; only button slots 4..12 (9 slots,
// one shared 4-bit selector each for both player ports) carry a selector. This
// is the always-free
// replacement for the old per-core hardcoded "Buttons Config." presets;
// layouts are user-defined via the OSD "Define DB9 buttons" flow and persisted
// per-core/per-devtype as .map files (see input.cpp).
//
// OSD-time only: the stream fires on core load and on DB9 device-type change.
// It NEVER runs in the input hot path (Critical Rule #2): the FPGA mux is
// combinational off a config register, so gameplay latency is unaffected.
#pragma once

#include <stdint.h>

// Output slot order = MiSTer-standard joystick word bit index:
//   0=Right 1=Left 2=Down 3=Up 4=Btn0 5=Btn1 6=Btn2 7=Btn3 8=Btn4 9=Btn5
//   10=Select 11=Start 12=Btn8 ... 15.
#define DB9_MAP_SLOTS  16

// Only output slots 4..12 (9 button slots) are remappable in the FPGA matrix;
// D-pad slots 0..3 are hardwired identity (skipped by the Define walk + stream).
#define DB9_MAP_BTN_FIRST 4
#define DB9_MAP_BTN_LAST  12

// Per-slot selector source values (match joydb_remap.sv pack_src(), 4-bit):
//   0..13 : physical raw joydb bit index (devtype-specific meaning, below)
#define DB9_MAP_COMBO_STARTB 14 // Start & B combo (Saturn Select when R is in use)
#define DB9_MAP_NONE   15   // unmapped -> constant 0

// DB9 device types (match db9_type_name() / FPGA joy_raw[15:14]):
//   1 = Saturn, 2 = DB9MD, 3 = DB15.
#define DB9_DEV_SATURN 1
#define DB9_DEV_DB9MD  2
#define DB9_DEV_DB15   3

// Physical raw bit layout per device type (joydb_1/joydb_2, active-high):
//   DB9MD : 0-3=UDLR 4=A 5=B 6=C 7=X 8=Y 9=Z 10=Start 11=Mode
//   DB15  : 0-3=UDLR 4=A 5=B 6=C 7=D 8=E 9=F 10=Start 11=Select
//   Saturn: 0-3=UDLR 4=A 5=B 6=C 7=X 8=Y 9=Z 10=Start 11=R 12=L

// Fill map[DB9_MAP_SLOTS] with a sensible factory layout for the given devtype.
// Used when no user .map exists yet. Derived from the core's CONF_STR J1 button
// labels (same-named pad button first, class alias second, jn/jp consulted only
// for labels that resolve to nothing), falling back to a hardcoded per-devtype
// table only when the core declares no real J1 button.
void db9_map_factory_default(int devtype, uint8_t *map);

// Pack map[] button slots 4..12 into the 6-byte selector table and stream it to
// the FPGA over UIO_DB9_MAP. One shared layout drives both player ports.
void db9_map_stream(const uint8_t *map);
// [MiSTer-DB9 END]
