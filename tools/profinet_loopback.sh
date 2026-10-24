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
# [B851] RESOLVE THE BINARY AND THE FS IMAGE THROUGH `w3_paths.sh`, NOT BY HARDCODED PATH.
# This script is wired into the `profibus` test_id, so its verdict is RECORDED as a cell --
# which is why it is fixed first.  The old `${ROOT}/.pio/build/${ENV_NAME}/program` has not
# been written by the harness since P204/B449 defaulted W3_ROLE to a per-session build tree.
# The failure was NOT "no binary": measured 2026-10-13, that path existed and was SIX DAYS
# OLD, so this script would have run last week's code and reported a verdict about today's.
# `${ROOT}/data_staged/${ENV_NAME}` was the second defect: that is the directory `w3_assemble`
# REWRITES, so a peer's build repopulates it mid-run -- exactly what B561 stopped the host
# tiers doing and what B850 then had to verify rather than trust.  `w3p_fsdir` hands back a
# private, verified copy, and ([B892]) sweeps it at exit itself, so no trap is needed here.
. "$(dirname "$0")/w3_paths.sh"
PROG="$(w3p_prog "${ENV_NAME}")"   || { echo "Total: 0 pass, 1 fail"; exit 1; }
FSDIR="$(w3p_fsdir "${ENV_NAME}")" || { echo "Total: 0 pass, 1 fail"; exit 1; }

fail() { echo "profinet_loopback: $*" >&2; echo "Total: 0 pass, 1 fail"; echo "--- profinet done ---"; exit 1; }
[ -f "${FSDIR}/ll_tests/profinet-tests.scm" ] || fail "profinet-tests.scm not staged"

TMP="$(mktemp -d)"
MOCK_PID=""
cleanup() { [ -n "${MOCK_PID}" ] && kill "${MOCK_PID}" 2>/dev/null; rm -rf "${TMP}"; }
#! [B892] `w3_atexit`, NOT A BARE `trap ... EXIT`.  Bash keeps ONE EXIT handler per shell, so a
#! bare `trap` here REPLACES every action already registered -- including the snapshot sweep
#! `w3_paths.sh` arms when it is sourced.  MEASURED 2026-10-16 on THIS SCRIPT: a PASSING run (`25 pass, 0 fail`, rc 0) left a 1.3 MB
#! `/tmp/w3p-fs-*` snapshot behind, because the `trap cleanup EXIT` below silently replaced the
#! `trap 'rm -rf "$(dirname "${FSDIR}")"' EXIT` written sixteen lines above it to prevent exactly
#! that.  Two traps, both correct in isolation, and the second one deleted the first.  Nothing
#! reported it: the test passed and the cell was recorded.
w3_atexit cleanup

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
