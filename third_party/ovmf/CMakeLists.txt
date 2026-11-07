# ==============================================================================
# OVMF: prebuilt UEFI firmware for QEMU, downloaded and checked at configure time.
# ==============================================================================

include(FetchContent)

set(EREBUS_OVMF_URL
    "https://github.com/osdev0/edk2-ovmf-stable-bins/releases/download/20261026T212116Z/edk2-ovmf-bins.tar.xz"
    CACHE STRING "Archive of prebuilt OVMF firmware")
set(EREBUS_OVMF_SHA256 "1619a6884c9380516152c2fe3c73fe1d7851014f3ec6a784b5d7b0324983b4d4"
    CACHE STRING "SHA-256 of that archive")

FetchContent_Declare(ovmf
    URL "${EREBUS_OVMF_URL}"
    URL_HASH "SHA256=${EREBUS_OVMF_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_NO_PROGRESS TRUE
)
FetchContent_MakeAvailable(ovmf)

set(OVMF_DIR "${ovmf_SOURCE_DIR}" PARENT_SCOPE)
message(STATUS "OVMF: ${ovmf_SOURCE_DIR}")
