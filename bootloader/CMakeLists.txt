# Blackroo Linux Bootloader — PSn00bSDK CMake project
cmake_minimum_required(VERSION 3.21)

project(blackroo-bootloader LANGUAGES C ASM)

# BLACKROO: -DBLACKROO_TEST_HIGH=ON links kloader at 0x80090000 instead of
# 0x80010000, so a *running* kloader can upload and launch it over serial for
# testing. At the normal address the upload would overwrite the loader doing
# the uploading, which is GR-008. Test artefact only - the disc build stays low.
if(BLACKROO_TEST_HIGH)
    set(PSN00BSDK_LDSCRIPTS ${CMAKE_CURRENT_SOURCE_DIR}/ldscripts-test)
    message(STATUS "BLACKROO: linking kloader HIGH at 0x80090000 (test build)")
endif()

psn00bsdk_add_executable(bootloader GPREL
    src/main.c
    src/menu.c
    src/hwinfo.c
    src/serial.c
    src/shell.c
    src/memcard.c
    src/kernel.c
    src/settings.c
    src/video.c
    src/logo.c
    src/logo_data.c
    src/pioflash.c
)

target_link_libraries(bootloader PRIVATE
    psxgpu      # GPU rendering
    psxgte      # GTE (geometry, not needed but included)
    psxspu      # Sound (mute on boot)

    psxcd       # CD-ROM access
    psxsio      # Serial I/O
    psxapi      # BIOS API
    c           # C standard library
)

# Output PS-EXE (CD image creation commented out until iso.xml exists)
#psn00bsdk_add_cd_image(bootloader-cd bootloader
#    ${CMAKE_CURRENT_SOURCE_DIR}/iso/iso.xml
#    OPTIONAL
#)
