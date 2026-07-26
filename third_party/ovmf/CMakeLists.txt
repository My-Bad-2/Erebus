cmake_minimum_required(VERSION 3.18)

set(OVMF_BINARY_URL "https://github.com/osdev0/edk2-ovmf-stable-bins/releases/download/20260526T165842Z/edk2-ovmf-bins.tar.xz"
		CACHE STRING "URL to the pre-built Ovmf binary archive")

set(OVMF_BINARY_SHA256 "2b1af03999f39625f20a6ef7e72c10eace3f0335a8646ed836f186f2c8e1b3e8"
		CACHE STRING "Expected SHA256 hash of the archive")

set(OVMF_ARCHIVE_FILE "${CMAKE_CURRENT_BINARY_DIR}/ovmf-binaries.tar.xz")
set(OVMF_EXTRACT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ovmf-extracted")

if (NOT EXISTS "${OVMF_EXTRACT_ROOT}")
	message(STATUS "Ovmf: Downloading pre-built binaries...")

	file(DOWNLOAD "${OVMF_BINARY_URL}" "${OVMF_ARCHIVE_FILE}"
			EXPECTED_HASH SHA256=${OVMF_BINARY_SHA256}
			STATUS DOWNLOAD_STATUS
	)

	list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
	list(GET DOWNLOAD_STATUS 1 STATUS_MSG)

	if (NOT STATUS_CODE EQUAL 0)
		message(FATAL_ERROR "Ovmf: Download failed with error: ${STATUS_MSG}")
	endif ()

	message(STATUS "Ovmf: Extracting archive...")

	file(ARCHIVE_EXTRACT
			INPUT "${OVMF_ARCHIVE_FILE}"
			DESTINATION "${OVMF_EXTRACT_ROOT}"
	)

	message("Ovmf: Extraction complete.")
endif ()

file(GLOB OVMF_INNER_DIR "${OVMF_EXTRACT_ROOT}/*")
set(OVMF_DIR "${OVMF_INNER_DIR}" PARENT_SCOPE)
set(OVMF_DIR "${OVMF_INNER_DIR}")
message(STATUS "Ovmf: Binaries available at -> ${OVMF_DIR}")