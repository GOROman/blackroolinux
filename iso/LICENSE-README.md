# Disc licence data — do not redistribute

`LICENSEE.DAT` (PAL/Europe) and `LICENSEA.DAT` (NTSC-U/America) are the 12
licence sectors a PlayStation expects in the first 16 sectors of a disc. Each
is 12 x 2336 bytes: an 8-byte Mode 2 subheader (`00 00 08 00 00 00 08 00`,
submode bit 3 = data) followed by the sector body, with the SCE logo data in
sectors 8-15 and a region marker inside sector 8.

**A console rejects a disc whose licence area is missing or malformed —
"Please insert PlayStation CD-ROM" — regardless of a modchip.** The modchip
answers the wobble/SCEx check; this is a separate check. Verified here on
2026-08-20: a disc built with 28,032 zero bytes was rejected, and the same
image with `LICENSEE.DAT` was accepted.

These files are Sony's data, taken from a disc Chelson owns (they came in via
`~/projects/psx-video/pinecore-player/`). They are here so discs built on this
machine boot on this machine's console. **Do not publish a `.bin` built with
them, and do not commit them to a public repository.** Anyone rebuilding from
a clean checkout should supply their own, extracted from a disc they own:

```bash
dumpsxiso -x out/ their-own-game.bin      # writes out/license_data.dat
```

`license.dat` (all zeros) remains as the no-licence placeholder: fine for
emulators, useless on hardware.
