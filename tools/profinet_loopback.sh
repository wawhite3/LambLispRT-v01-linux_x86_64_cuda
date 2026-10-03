#!/bin/bash
# Copyright 2026 by Frobenius Norm LLC 2026-08-20 23:50:00
# Free for non-commercial use. Commercial use requires a license.
#
# profinet_loopback.sh -- P145 Part B, LambLisp-side PROFINET test.
#
# Starts the MOCK daemon (w3_ai_scripts/profinet_mock_daemon.py) and runs
# profinet-tests.scm against it.  Proves the whole LambLisp half -- IPC
# transport, device model, process image, IOPS/IOCS, events, stats, error
# mapping -- with NO p-net, NO GPLv3 code, no controller and no network.
#
# What this does NOT prove: any actual PROFINET wire behaviour.  That needs the
# real daemon (w3_profinet/) plus an IO-Controller, and is P145 test case 15.
#
# Usage: w3_ai_scripts/profinet_loopback.sh [env]
set -u
ENV_NAME="${1:-linux_x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROG="${ROOT}/.pio/build/${ENV_NAME}/program"
FSDIR="${ROOT}/data_staged/${ENV_NAME}"

fail() { echo "profinet_loopback: $*" >&2; echo "Total: 0 pass, 1 fail"; echo "--- profinet done ---"; exit 1; }
[ -x "${PROG}" ]  || fail "no binary at ${PROG} -- ./w3 make one ${ENV_NAME}"
[ -d "${FSDIR}" ] || fail "no staged FS -- ./w3 make assemble ${ENV_NAME}"
[ -f "${FSDIR}/ll_tests/profinet-tests.scm" ] || fail "profinet-tests.scm not staged"

TMP="$(mktemp -d)"
MOCK_PID=""
cleanup() { [ -n "${MOCK_PID}" ] && kill "${MOCK_PID}" 2>/dev/null; rm -rf "${TMP}"; }
trap cleanup EXIT

# The scripted DCP sequence is the order a real engineering tool commissions in.
cat > "${TMP}/events.txt" <<'SCRIPT'
set-name lamb-io-1
set-ip 192.168.0.50 255.255.255.0 192.168.0.1
connect
param-end
app-ready
SCRIPT

python3 "${ROOT}/w3_ai_scripts/profinet_mock_daemon.py" \
        --path "${TMP}/pn.sock" --script "${TMP}/events.txt" --quiet \
        > "${TMP}/port.txt" 2>"${TMP}/mock.err" &
MOCK_PID=$!

for _ in $(seq 1 50); do [ -S "${TMP}/pn.sock" ] && break; sleep 0.1; done
[ -S "${TMP}/pn.sock" ] || fail "mock daemon did not bind (see ${TMP}/mock.err)"

out="$(cd "${FSDIR}" && \
  printf '(define pn-sock "%s")\n(load "ll_tests/profinet-tests.scm" 0)\n' "${TMP}/pn.sock" \
  | timeout 90 "${PROG}" 2>&1)"

echo "${out}" | grep -aE '^\s*(PASS|FAIL)' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
TOTAL="$(echo "${out}" | grep -aoE 'Total: [0-9]+ pass, [0-9]+ fail' | tail -1)"
[ -n "${TOTAL}" ] || fail "no Total line from profinet-tests.scm"
echo "${TOTAL}"
echo "--- profinet done ---"
NFAIL="$(echo "${TOTAL}" | sed -E 's/.*, ([0-9]+) fail/\1/')"
[ "${NFAIL}" = "0" ]
