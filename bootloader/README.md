# Blackroo Bootloader

> PSn00bSDK-based first-stage bootloader for PlayStation 1

**Status:** Not yet started

This will be a PS1 executable (PS-EXE) that provides a boot menu for loading
the Linux kernel from multiple sources: serial upload, memory card, or CD-ROM.

See `docs/06-BOOTLOADER-DESIGN.md` for the full design document.

## Build Requirements

- PSn00bSDK (via Docker container or local install)
- See `docker/Dockerfile.psn00bsdk` for the build environment

## Design

Inspired by the [PS2 kernelloader](https://sourceforge.net/p/kernelloader/kernelloader/ci/master/tree/) project, adapted for PS1 hardware.
