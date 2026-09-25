# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

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
