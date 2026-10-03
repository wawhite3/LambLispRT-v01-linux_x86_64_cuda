#!/bin/bash
# Copyright 2026 by Frobenius Norm LLC 2026-08-22
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the GPLv3 PROFINET daemon side: p-net core + osal + our pnal port.
# Produces libprofinet.a and a link-proof binary that calls pnet_init().
#
# NOTHING BUILT HERE MAY BE LINKED INTO liblamblisp.a.  See README.md.
# w3_make_release_one aborts if p-net symbols ever reach a shipped package.
#
# The public p-net drop has NO CMakeLists, so this script IS the build.
#
# Usage:  w3_profinet/build.sh [srcdir]      (default: ./extern)
#   srcdir must contain  p-net/  and  osal/  cloned from:
#     git clone --depth 1 https://github.com/rtlabs-com/p-net.git
#     git clone --depth 1 https://github.com/rtlabs-com/osal.git
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-${HERE}/extern}"
OUT="${HERE}/build"
PNAL="${HERE}/pnal"

[ -d "${SRC}/p-net" ] || { echo "build.sh: no ${SRC}/p-net -- clone it (see header)"; exit 1; }
[ -d "${SRC}/osal" ]  || { echo "build.sh: no ${SRC}/osal -- clone it (see header)";  exit 1; }

mkdir -p "${OUT}/obj" "${OUT}/gen"

# The CMake-templated headers, expanded by hand because there is no CMake.
# pnet_options.h.example is the working option set -- see README.md for the two
# values that have hard ceilings (ALARM_PAYLOAD <= 1408, DIAG_MANUF <= 1396).
cp "${PNAL}/pnet_options.h.example" "${OUT}/gen/pnet_options.h"
# #cmakedefine must become a real #define, not survive as a directive.
sed -e 's/@[A-Za-z_]*@/1/g' \
    -e 's/^#cmakedefine01 \([A-Za-z_][A-Za-z0-9_]*\)/#define \1 0/' \
    -e 's/^#cmakedefine \([A-Za-z_][A-Za-z0-9_]*\)/#define \1/' \
    "${SRC}/p-net/pnet_version.h.in" > "${OUT}/gen/pnet_version.h"
cat > "${OUT}/gen/pnet_export.h" <<'HDR'
#ifndef PNET_EXPORT_H
#define PNET_EXPORT_H
#define PNET_EXPORT
#define PNET_NO_EXPORT
#endif
HDR

INC="-I ${SRC}/p-net/include -I ${SRC}/p-net/src -I ${SRC}/p-net/src/common"
INC="${INC} -I ${SRC}/p-net/src/device -I ${SRC}/p-net/src/drivers -I ${OUT}/gen"
INC="${INC} -I ${SRC}/osal/include -I ${SRC}/osal/src/linux -I ${SRC}/osal/src/linux/sys -I ${PNAL}"

rm -f "${OUT}/obj"/*.o
# pf_snmp.c is excluded: PNET_OPTION_SNMP=0 (SNMP/LLDP-MIB is Conformance
# Class B; CC-A does not need it), which is what the CMake build would do.
for f in $(ls "${SRC}"/p-net/src/common/*.c "${SRC}"/p-net/src/device/*.c | grep -v pf_snmp.c) \
         "${SRC}"/osal/src/linux/osal.c "${SRC}"/osal/src/linux/osal_log.c \
         "${PNAL}/pnal_linux.c"; do
    o="${OUT}/obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
    gcc -c -std=gnu99 -O2 -Wall ${INC} "$f" -o "$o"
done
ar rcs "${OUT}/libprofinet.a" "${OUT}/obj"/*.o
echo "build.sh: ${OUT}/libprofinet.a  ($(ls "${OUT}/obj"/*.o | wc -l) objects)"

# The daemon itself.  -I ../src for ll_vm_profinet_ipc.h: the IPC protocol
# header is shared with the LambLisp side and is NOT GPLv3 -- it is the
# interface definition, which is exactly what the p-net licence carve-out
# contemplates.  No p-net header is visible from the LambLisp side.
if [ -f "${HERE}/pnd_main.c" ]; then
    gcc -std=gnu99 ${INC} -I "${HERE}/../src" "${HERE}/pnd_main.c" \
        "${OUT}/libprofinet.a" -lpthread -lrt -o "${OUT}/pnd"
    echo "build.sh: ${OUT}/pnd -- the daemon"
fi

if [ -f "${HERE}/linktest.c" ]; then
    gcc -std=gnu99 ${INC} "${HERE}/linktest.c" "${OUT}/libprofinet.a" \
        -lpthread -lrt -o "${OUT}/linktest"
    echo "build.sh: ${OUT}/linktest -- run it to call pnet_init()"
    echo "build.sh: raw Ethernet needs CAP_NET_RAW:"
    echo "          sudo setcap cap_net_raw+ep ${OUT}/linktest"
fi
