#!/bin/bash
#
# Copyright 2015 The Bazel Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# OS X relpath is not really working. This is a wrapper script around gcc
# to simulate relpath behavior.
#
# This wrapper uses install_name_tool to replace all paths in the binary
# (bazel-out/.../path/to/original/library.so) by the paths relative to
# the binary. It parses the command line to behave as rpath is supposed
# to work.
#
# See https://blogs.oracle.com/dipol/entry/dynamic_libraries_rpath_and_mac
# on how to set those paths for Mach-O binaries.
#
set -eu

GCC=/usr/local/bin/gcc-7
INSTALL_NAME_TOOL="/usr/bin/install_name_tool"

LIBS=
LIB_DIRS=
RPATHS=
OUTPUT=
# let parse the option list
for i in "$@"; do
    if [[ "${OUTPUT}" = "1" ]]; then
        OUTPUT=$i
    elif [[ "$i" =~ ^-l(.*)$ ]]; then
        LIBS="${BASH_REMATCH[1]} $LIBS"
    elif [[ "$i" =~ ^-L(.*)$ ]]; then
        LIB_DIRS="${BASH_REMATCH[1]} $LIB_DIRS"
    elif [[ "$i" =~ ^-Wl,-rpath,\@loader_path/(.*)$ ]]; then
        RPATHS="${BASH_REMATCH[1]} ${RPATHS}"
    elif [[ "$i" = "-o" ]]; then
        # output is coming
        OUTPUT=1
    fi
done

# Call gcc
PATH="/usr/local/Cellar/gcc/7.3.0/libexec/gcc/x86_64-apple-darwin17.3.0/7.3.0:$PATH" ${GCC} "$@"

function get_library_path() {
    for libdir in ${LIB_DIRS}; do
        if [ -f ${libdir}/lib$1.so ]; then
            echo "${libdir}/lib$1.so"
        elif [ -f ${libdir}/lib$1.dylib ]; then
            echo "${libdir}/lib$1.dylib"
        fi
    done
}

# A convenient method to return the actual path even for non symlinks
# and multi-level symlinks.
function get_realpath() {
    local previous="$1"
    local next=$(readlink "${previous}")
    while [ -n "${next}" ]; do
        previous="${next}"
        next=$(readlink "${previous}")
    done
    echo "${previous}"
}

# Get the path of a lib inside a tool
function get_otool_path() {
    # the lib path is the path of the original lib relative to the workspace
    get_realpath $1 | sed 's|^.*/bazel-out/|bazel-out/|'
}

# Do replacements in the output
for rpath in ${RPATHS}; do
    for lib in ${LIBS}; do
        unset libname
        if [ -f "$(dirname ${OUTPUT})/${rpath}/lib${lib}.so" ]; then
            libname="lib${lib}.so"
        elif [ -f "$(dirname ${OUTPUT})/${rpath}/lib${lib}.dylib" ]; then
            libname="lib${lib}.dylib"
        fi
        # ${libname-} --> return $libname if defined, or undefined otherwise. This is to make
        # this set -e friendly
        if [[ -n "${libname-}" ]]; then
            libpath=$(get_library_path ${lib})
            if [ -n "${libpath}" ]; then
                ${INSTALL_NAME_TOOL} -change $(get_otool_path "${libpath}") \
                    "@loader_path/${rpath}/${libname}" "${OUTPUT}"
            fi
        fi
    done
done
