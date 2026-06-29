# FindLibVLC - locate libVLC. (libVLC is NOT available via vcpkg.)
#   Linux/macOS: via pkg-config.
#   Windows: searches the VLC SDK -- set LIBVLC_SDK_DIR/VLC_SDK_DIR, else looks in
#            the standard VLC install (C:/Program Files/VideoLAN/VLC/sdk).
# Provides: LIBVLC_FOUND and the imported target LibVLC::LibVLC.
if(TARGET LibVLC::LibVLC)
  set(LIBVLC_FOUND TRUE)
  return()
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(_LIBVLC QUIET libvlc)
endif()

set(_vlc_hints
  "$ENV{LIBVLC_SDK_DIR}" "${LIBVLC_SDK_DIR}"
  "$ENV{VLC_SDK_DIR}"    "${VLC_SDK_DIR}"
  "C:/Program Files/VideoLAN/VLC/sdk"
  "C:/Program Files (x86)/VideoLAN/VLC/sdk")

find_path(LIBVLC_INCLUDE_DIR
  NAMES vlc/vlc.h
  HINTS ${_LIBVLC_INCLUDEDIR} ${_LIBVLC_INCLUDE_DIRS} ${_vlc_hints}
  PATH_SUFFIXES include)

find_library(LIBVLC_LIBRARY
  NAMES vlc libvlc
  HINTS ${_LIBVLC_LIBDIR} ${_LIBVLC_LIBRARY_DIRS} ${_vlc_hints}
  PATH_SUFFIXES lib)

find_library(LIBVLCCORE_LIBRARY
  NAMES vlccore libvlccore
  HINTS ${_LIBVLC_LIBDIR} ${_LIBVLC_LIBRARY_DIRS} ${_vlc_hints}
  PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibVLC REQUIRED_VARS LIBVLC_LIBRARY LIBVLC_INCLUDE_DIR)

if(LibVLC_FOUND)
  add_library(LibVLC::LibVLC UNKNOWN IMPORTED)
  set_target_properties(LibVLC::LibVLC PROPERTIES
    IMPORTED_LOCATION "${LIBVLC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBVLC_INCLUDE_DIR}")
  if(LIBVLCCORE_LIBRARY)
    set_property(TARGET LibVLC::LibVLC APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${LIBVLCCORE_LIBRARY}")
  endif()
endif()
set(LIBVLC_FOUND ${LibVLC_FOUND})
