# ==============================================================================
# Limine: prebuilt bootloader binaries, downloaded and checked at configure time.
# ==============================================================================

include(FetchContent)

set(EREBUS_LIMINE_URL
    "https://github.com/Limine-Bootloader/Limine/releases/download/v12.9.6/limine-binary.tar.gz"
    CACHE STRING "Archive of prebuilt Limine binaries")
set(EREBUS_LIMINE_SHA256 "b38aabb042b0c0f9f562b7741be3c1204838d7bbacfa78bb411c674bd5b984ee"
    CACHE STRING "SHA-256 of that archive")

FetchContent_Declare(limine
    URL "${EREBUS_LIMINE_URL}"
    URL_HASH "SHA256=${EREBUS_LIMINE_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_NO_PROGRESS TRUE
)
FetchContent_MakeAvailable(limine)

set(LIMINE_DIR "${limine_SOURCE_DIR}" PARENT_SCOPE)
message(STATUS "Limine: ${limine_SOURCE_DIR}")
