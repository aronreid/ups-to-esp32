# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Every board builds and ships on ESP-IDF 5.5 (5.5.5 in CI): the newest line
# before 6.0's breaking changes (mbedTLS 4 and the PSA crypto API, which
# tlscert and the vendored MicroLink call directly; picolibc). One version, so
# the image tested on the bench is the image released. Moving to 6.x is its
# own piece of work: change this check with it, not before.
if(NOT IDF_VERSION_MAJOR EQUAL 5 OR IDF_VERSION_MINOR LESS 5)
    message(FATAL_ERROR "This firmware is built with ESP-IDF 5.5.x; this is "
                        "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}. See firmware/README.md.")
endif()

# The project name is stamped into the image's app descriptor, and the OTA
# client REFUSES an image whose name does not match its own. That makes the
# board identity part of the firmware rather than part of the filename -- a
# correctly named file containing the wrong build is exactly what shipped once,
# and a filename is a label anyone can get wrong.
#
# Set by the build:  -DUPSA_IMAGE_NAME=ups-adaptor-reva
# Left unset it falls back to the plain name, which no released image uses.
if(NOT DEFINED UPSA_IMAGE_NAME)
    set(UPSA_IMAGE_NAME "ups-adaptor")
endif()
project(${UPSA_IMAGE_NAME})
