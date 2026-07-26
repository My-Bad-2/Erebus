cmake_minimum_required(VERSION 3.18)

set(LIMINE_BINARY_URL "https://github.com/Limine-Bootloader/Limine/releases/download/v12.5.2/limine-binary.tar.xz"
		CACHE STRING "URL to the pre-built Limine binary archive")

set(LIMINE_BINARY_SHA256 "5e2d6eb86623fcdcd2a873c9eca7dcafccb34182c17779535fe824fd57b688c5"
		CACHE STRING "Expected SHA256 hash of the archive")

set(LIMINE_ARCHIVE_FILE "${CMAKE_CURRENT_BINARY_DIR}/limine-binaries.tar.xz")
set(LIMINE_EXTRACT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/limine-extracted")

if (NOT EXISTS "${LIMINE_EXTRACT_ROOT}")
	message(STATUS "Limine: Downloading pre-built binaries...")

	file(DOWNLOAD "${LIMINE_BINARY_URL}" "${LIMINE_ARCHIVE_FILE}"
			EXPECTED_HASH SHA256=${LIMINE_BINARY_SHA256}
			STATUS DOWNLOAD_STATUS
	)

	list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
	list(GET DOWNLOAD_STATUS 1 STATUS_MSG)

	if (NOT STATUS_CODE EQUAL 0)
		message(FATAL_ERROR "Limine: Download failed with error: ${STATUS_MSG}")
	endif ()

	message(STATUS "Limine: Extracting archive...")

	file(ARCHIVE_EXTRACT
			INPUT "${LIMINE_ARCHIVE_FILE}"
			DESTINATION "${LIMINE_EXTRACT_ROOT}"
	)

	message("Limine: Extraction complete.")
endif ()

file(GLOB LIMINE_INNER_DIR "${LIMINE_EXTRACT_ROOT}/*")
set(LIMINE_DIR "${LIMINE_INNER_DIR}" PARENT_SCOPE)
set(LIMINE_DIR "${LIMINE_INNER_DIR}")
message(STATUS "Limine: Binaries available at -> ${LIMINE_DIR}")

