/*
 * version.h — Blackroo Bootloader version info
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_VERSION_H
#define BLACKROO_VERSION_H

#define BLACKROO_VERSION_MAJOR  0
#define BLACKROO_VERSION_MINOR  5
#define BLACKROO_VERSION_PATCH  0
#define BLACKROO_VERSION        "0.5.0"
#define BLACKROO_CODENAME       "Rootstock26"

/*
 * 0.5 "Rootstock26", 2026-08-26 — the release where the disc carries the root
 * filesystem. A 96 KB kernel size pass paid for a 192 KB userspace window;
 * brsh grew the file commands; psxcd finds ROOT.IMG by walking the ISO9660
 * directory, so the disc can be rebuilt without rebuilding Linux. Nothing
 * outside the console is involved: BIOS -> kloader -> LINUX.EXE -> the disc's
 * own filesystem -> a shell on the television.
 *
 * 0.4 "Untethered", 2026-08-22 — the release where the host PC stopped being
 * required: a keyboard on the controller bus, output on the television, and
 * the machine driving itself.
 *
 * 0.3 "Phosphor", 2026-08-21 — the release where the shell reached the screen.
 * This string is drawn on kloader's menu and its serial shell, so a burned disc
 * keeps showing whatever version it was built with: reburn to see this one.
 */

#endif
