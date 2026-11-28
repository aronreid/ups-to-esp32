# SPDX-License-Identifier: GPL-3.0-or-later
#
# lwIP fixes that no ESP-IDF release carries yet (firmware/patches/README.md).
#
# The files under firmware/lwip-patched are ESP-IDF 5.5.6's own lwIP sources
# with the lwip-*.patch files in firmware/patches applied. Included from
# firmware/CMakeLists.txt after project(): by then IDF's lwip component has
# registered its library, and this swaps each of its listed sources for the
# patched copy in that library's source list. Same library, same compile
# options and include paths, same object names (so the linker fragments that
# place tcp_in functions still match); only the file read changes.
#
# Each IDF original is pinned by its SHA-256. On any other IDF the build stops
# here rather than compile a 5.5.6-based copy against a different lwIP: re-make
# the copies from the new IDF (firmware/patches/README.md), or drop a patch the
# new IDF already carries. tools/test/test_lwip_patch.py checks the copies are
# exactly IDF 5.5.6 + the patches, and that a build compiled them.

set(UPSA_LWIP_PATCHED
    # file in the lwip component                IDF 5.5.6 original's SHA-256
    "lwip/src/core/tcp_in.c|a6169a86b5cbc244fbc8029333b99bfb7bb81d9644d2f235114c7f2cb0bc3bf5"
    "lwip/src/core/ipv6/nd6.c|e35fb0d155e6545635f65968f5a5082248287264c5e32cbb243ee564d13a5c04"
)

idf_component_get_property(upsa_lwip_lib lwip COMPONENT_LIB)
idf_component_get_property(upsa_lwip_dir lwip COMPONENT_DIR)
get_target_property(upsa_lwip_srcs ${upsa_lwip_lib} SOURCES)
foreach(entry ${UPSA_LWIP_PATCHED})
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 rel)
    list(GET entry 1 want)
    set(orig "${upsa_lwip_dir}/${rel}")
    set(ours "${CMAKE_CURRENT_LIST_DIR}/${rel}")
    file(SHA256 "${orig}" have)
    if(NOT have STREQUAL want)
        message(FATAL_ERROR "firmware/lwip-patched/${rel} is ESP-IDF 5.5.6's ${rel} with the "
                            "firmware/patches/lwip-*.patch fixes, but this IDF's ${orig} is a "
                            "different file. Re-make it for this IDF (firmware/patches/README.md).")
    endif()
    list(FIND upsa_lwip_srcs "${orig}" i)
    if(i EQUAL -1)
        message(FATAL_ERROR "IDF's lwip component does not build ${rel} (lwip-patched.cmake)")
    endif()
    list(REMOVE_AT upsa_lwip_srcs ${i})
    list(INSERT upsa_lwip_srcs ${i} "${ours}")
endforeach()
set_property(TARGET ${upsa_lwip_lib} PROPERTY SOURCES ${upsa_lwip_srcs})
