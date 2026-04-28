// [MiSTer-DB9-Pro BEGIN] - key gating v1.5 (per-customer SipHash MAC)
// FPGA contract: Main_MiSTer reads /media/fat/db9pro.key (64B), verifies the
// SipHash-2-4 MAC over the signed-payload range, then streams 40B
// (signed-payload || auth_tag) to hps_io via UIO_DB9_KEY (0xFE). The FPGA
// recomputes the MAC against its synth-time MASTER_ROOT and, on match, drives
// a `feature_mask` whose bits feed `saturn_unlocked` / future
// per-feature unlocks. Cores AND those wires into their gated logic
// (Saturn UserIO, ...). The key file is not in
// status[] — status is OSD/CONF_STR-controlled and saved per-core, neither
// of which applies to a HPS-side licensing flag.
//
// Secret injection: db9_key_secret.h (gitignored) is materialised at
// build time from the GitHub Org secret MASTER_ROOT_HEX (64 hex chars).
// Public clones without that secret build a no-op stub via __has_include
// and Saturn stays permanently locked.
#pragma once

void db9_key_refresh(void);          // re-read /media/fat/db9pro.key, verify, drive FPGA gate
int  db9_key_saturn_unlocked(void);  // 1 = Saturn unlocked
// [MiSTer-DB9-Pro END]
