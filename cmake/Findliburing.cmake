find_path(
  liburing_INCLUDE_DIR
  NAMES
    liburing.h
)

find_library(
  liburing_LIBRARY
  NAMES
    uring
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  liburing
  REQUIRED_VARS
    liburing_INCLUDE_DIR
    liburing_LIBRARY
)

if(liburing_FOUND AND NOT TARGET liburing::liburing)
  add_library(liburing::liburing UNKNOWN IMPORTED)
  set_target_properties(
    liburing::liburing
    PROPERTIES
      IMPORTED_LOCATION
        "${liburing_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES
        "${liburing_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  liburing_INCLUDE_DIR
  liburing_LIBRARY
)
