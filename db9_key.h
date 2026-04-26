// [MiSTer-DB9-Pro BEGIN] - key gating for Saturn User Joystick
// FPGA contract: Main_MiSTer broadcasts the unlock bit via UIO_DB9_KEY (0xFE),
// which lands on hps_io's `saturn_unlocked` output port. Cores AND it with
// `joy_saturn_en` so Saturn mode is silent no-op when the key file is
// missing or invalid. The bit is intentionally NOT in the status[] word —
// status is OSD/CONF_STR-controlled and saved per-core, neither of which
// applies to a HPS-side licensing flag.
#pragma once

void db9_key_refresh();         // re-read /media/fat/db9pro.key; cache result
int  db9_key_saturn_unlocked(); // 1 = unlocked, 0 = locked / missing / invalid
// [MiSTer-DB9-Pro END]
