# Cross build of the Linux x86-64 game from Windows with LLVM clang and lld (docs/linux.md).
# PT_LINUX_SYSROOT: a Debian trixie amd64 sysroot (headers and libraries), made by tools/linux/make_sysroot.py.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT PT_LINUX_SYSROOT)
  set(PT_LINUX_SYSROOT "$ENV{PT_LINUX_SYSROOT}")
endif()
if(NOT PT_LINUX_SYSROOT)
  message(FATAL_ERROR "set PT_LINUX_SYSROOT to the Debian sysroot (tools/linux/make_sysroot.py)")
endif()
set(CMAKE_SYSROOT "${PT_LINUX_SYSROOT}")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PT_LINUX_SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH "${PT_LINUX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PT_LINUX_SYSROOT}")
# pkgconf on Windows splits the path list at ';' (a ':' breaks "C:/...") and moves a .pc file's prefix to the folder
# above its own unless told not to; without these SDL found no wayland, pulse or pipewire package (1.0.1 had none of them)
if(CMAKE_HOST_WIN32)
  set(ENV{PKG_CONFIG_LIBDIR} "${PT_LINUX_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig;${PT_LINUX_SYSROOT}/usr/share/pkgconfig")
  set(ENV{PKG_CONFIG_DONT_DEFINE_PREFIX} 1)
else()
  set(ENV{PKG_CONFIG_LIBDIR} "${PT_LINUX_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${PT_LINUX_SYSROOT}/usr/share/pkgconfig")
endif()

# SDL3 loads X11, Wayland, PulseAudio, PipeWire, ALSA, libdecor, xkbcommon and libusb with dlopen, by the file name its
# find_library returned. The sysroot holds copies instead of symlinks, so that name was the unversioned libX11.so that
# only exists with the -dev packages (1.0.1 failed with "No available video device" on systems without them). Giving
# SDL the versioned soname file (libX11.so.6) makes it record the name every system has.
foreach(_entry X11_LIB:libX11.so XEXT_LIB:libXext.so XCURSOR_LIB:libXcursor.so XI_LIB:libXi.so XFIXES_LIB:libXfixes.so
        XRANDR_LIB:libXrandr.so XRENDER_LIB:libXrender.so XSS_LIB:libXss.so XTST_LIB:libXtst.so ASOUND_LIB:libasound.so
        PULSE_LIB:libpulse.so PIPEWIRE_0.3_LIB:libpipewire-0.3.so WAYLAND_CLIENT_LIB:libwayland-client.so
        WAYLAND_EGL_LIB:libwayland-egl.so WAYLAND_CURSOR_LIB:libwayland-cursor.so XKBCOMMON_LIB:libxkbcommon.so
        DECOR_0_LIB:libdecor-0.so LibUSB_LIBRARY:libusb-1.0.so)
  string(REPLACE ":" ";" _entry "${_entry}")
  list(GET _entry 0 _var)
  list(GET _entry 1 _lib)
  file(GLOB _soname_files "${PT_LINUX_SYSROOT}/usr/lib/x86_64-linux-gnu/${_lib}.[0-9]")
  if(_soname_files AND NOT DEFINED ${_var})
    list(GET _soname_files 0 _soname_file)
    set(${_var} "${_soname_file}" CACHE FILEPATH "versioned soname of ${_lib} for SDL3's dlopen")
  endif()
endforeach()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
