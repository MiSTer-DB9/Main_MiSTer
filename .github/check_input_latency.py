#!/usr/bin/env python3
# Critical Rule #2 enforcement: NO latency in the controller input hot path.
#
# MiSTer-DB9's whole value proposition is zero-latency direct controller
# input. the fork latency notes forbids sleeps / blocking I/O / heap alloc
# / logging / locks anywhere between the controller read and the core
# receiving input — yet nothing enforced it. Commit a48de20 had to rip
# remove()/fopen blocking syscalls back out of input_cb() + the poll loop;
# a re-regression compiles and ships clean.
#
# This is a REGRESSION-DELTA gate, not a flat grep (the same contract
# merge_validate.sh uses): it scans only an allowlist of hot-path FUNCTION
# BODIES (extracted by brace-matching from the signature, NOT line ranges —
# line numbers rot on every upstream auto-merge) and fails only on a
# forbidden token that is NEWLY present versus a baseline. Pre-existing
# debug code (e.g. `if (stick_debug) printf(...)`) and setup-only functions
# (input_test's open()/ioctl init half, db9_shm_init's mmap/shm_open) never
# trip it — input_test and db9_shm_init are intentionally NOT in the
# allowlist; only input_poll's per-frame callees are.
#
#   check_input_latency.py baseline                 # snapshot pre-merge set
#   check_input_latency.py check                    # fail iff a token added
#   check_input_latency.py check --ref <gitref>     # baseline from a gitref
#                                                     (PR base / push before)
#
# Exit: 0 = no regression / no resolvable baseline (fail-open, exactly like
#       merge_validate's "no baseline" tier), 1 = a forbidden token was
#       introduced in a hot-path function, 2 = layout/parse error.

import os
import re
import subprocess
import sys
import tempfile

# file -> hot-path function names. input_test is EXCLUDED (its setup half
# legitimately open()/ioctl()s; the hot half is input_poll's callees).
# db9_shm_init is EXCLUDED (mmap/shm_open is one-time init, not the path).
HOT = {
    "input.cpp": (
        "input_poll", "input_cb", "build_joy_mask", "build_autofire_mask",
    ),
    "user_io.cpp": (
        "user_io_digital_joystick", "user_io_l_analog_joystick",
        "user_io_r_analog_joystick", "user_io_joyraw_check_change",
        "db9_shm_read", "db9_shm_write", "db9_shm_clear",
    ),
}

# Forbidden in a hot-path body. Each entry is a compiled pattern; the label
# is what gets reported / delta-compared.
FORBIDDEN = [
    (re.compile(r"\busleep\s*\("),            "usleep"),
    (re.compile(r"\bnanosleep\s*\("),         "nanosleep"),
    (re.compile(r"\bsleep_for\b"),            "sleep_for"),
    (re.compile(r"\bsleep\s*\("),             "sleep"),
    (re.compile(r"\bmalloc\s*\("),            "malloc"),
    (re.compile(r"\bcalloc\s*\("),            "calloc"),
    (re.compile(r"\brealloc\s*\("),           "realloc"),
    (re.compile(r"\bnew\b"),                  "new"),
    (re.compile(r"\bfopen\s*\("),             "fopen"),
    (re.compile(r"\bremove\s*\("),            "remove"),
    (re.compile(r"\brename\s*\("),            "rename"),
    (re.compile(r"\bmkdir\s*\("),             "mkdir"),
    (re.compile(r"\bprintf\s*\("),            "printf"),
    (re.compile(r"\bfprintf\s*\("),           "fprintf"),
    (re.compile(r"\bsprintf\s*\("),           "sprintf"),
    (re.compile(r"\bsnprintf\s*\("),          "snprintf"),
    (re.compile(r"\bstd::string\b"),          "std::string"),
    (re.compile(r"\bstd::vector\b"),          "std::vector"),
    (re.compile(r"\bstd::cout\b"),            "std::cout"),
    (re.compile(r"\bstd::cerr\b"),            "std::cerr"),
    (re.compile(r"\bpthread_mutex_lock\s*\("), "pthread_mutex_lock"),
    (re.compile(r"\bstd::mutex\b"),           "std::mutex"),
    (re.compile(r"\bstd::lock_guard\b"),      "std::lock_guard"),
]

BASELINE_FILE = os.path.join(
    os.environ.get("RUNNER_TEMP") or tempfile.gettempdir(),
    "db9_input_latency_baseline.txt")


def _strip(src):
    """Drop // and /* */ comments and "..."/'...' literals so a 'sleep' in a
    comment or a log format string is not mistaken for a forbidden call."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == "//":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif two == "/*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "\"'":
            i += 1
            while i < n and src[i] != c:
                i += 2 if src[i] == "\\" else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _body(src, name):
    """Brace-matched body text of the DEFINITION of `name` (skips a forward
    declaration, which hits `;` before `{`), or None if not defined here."""
    sig = re.compile(r"^[A-Za-z_][^\n]*\b" + re.escape(name) + r"\s*\(",
                      re.MULTILINE)
    for m in sig.finditer(src):
        i, n = m.start(), len(src)
        while i < n and src[i] not in "{;":
            i += 1
        if i >= n or src[i] == ";":
            continue  # forward declaration; keep looking for the definition
        depth, start = 0, i
        while i < n:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    return src[start:i + 1]
            i += 1
        return src[start:]  # unbalanced (parse error upstream) — caller copes
    return None


def _tokens_from_text(fname, text):
    """Set of `file::func::token` for every forbidden hit in the file's
    hot-path function bodies. Missing function = silently skipped (an
    upstream rename is not a latency regression; the delta handles churn)."""
    toks = set()
    src = _strip(text)
    for fn in HOT[fname]:
        body = _body(src, fn)
        if body is None:
            continue
        for pat, label in FORBIDDEN:
            if pat.search(body):
                toks.add(f"{fname}::{fn}::{label}")
    return toks


def _tokens_worktree():
    toks = set()
    for fname in HOT:
        if not os.path.isfile(fname):
            print(f"input-latency: {fname} not found in cwd "
                  f"(run from repo root)", file=sys.stderr)
            return None
        toks |= _tokens_from_text(fname, open(fname, errors="replace").read())
    return toks


def _tokens_at_ref(ref):
    """Token set of the HOT files as they were at <gitref>, or None if the
    ref / files cannot be resolved (caller is fail-open on None)."""
    toks = set()
    for fname in HOT:
        try:
            text = subprocess.run(
                ["git", "show", f"{ref}:{fname}"],
                capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, OSError):
            return None
        toks |= _tokens_from_text(fname, text)
    return toks


def main(argv):
    mode = argv[1] if len(argv) > 1 else ""
    if mode not in ("baseline", "check"):
        print("usage: check_input_latency.py {baseline|check [--ref <r>]}",
              file=sys.stderr)
        return 2

    cur = _tokens_worktree()
    if cur is None:
        return 2

    if mode == "baseline":
        with open(BASELINE_FILE, "w") as f:
            f.write("\n".join(sorted(cur)))
        print(f"input-latency: pre-merge baseline "
              f"({len(cur)} pre-existing token(s) recorded)")
        return 0

    # check
    base = None
    if os.path.isfile(BASELINE_FILE):
        with open(BASELINE_FILE) as f:
            base = {ln for ln in f.read().splitlines() if ln}
    elif "--ref" in argv:
        ref = argv[argv.index("--ref") + 1]
        base = _tokens_at_ref(ref)

    if base is None:
        print("input-latency: no resolvable baseline — skipping regression "
              "gate (fail-open).", file=sys.stderr)
        return 0

    new = sorted(cur - base)
    if not new:
        print("input-latency: OK (no forbidden token introduced in the "
              "input hot path).")
        return 0

    print("LATENCY REGRESSION IN Main_MiSTer INPUT HOT PATH", file=sys.stderr)
    print("Critical Rule #2 (the fork latency notes): these forbidden "
          "constructs were NEWLY introduced into hot-path functions:",
          file=sys.stderr)
    for t in new:
        fname, fn, tok = t.split("::")
        print(f"  {fname}: {fn}(): +{tok}", file=sys.stderr)
    print("Move the work off the controller-read -> core-input path, or "
          "justify with benchmarks per the rule.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
