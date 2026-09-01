#!/bin/sh
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# Adds the loopback aliases required by the OCUDU macOS test suite (see
# tests/ci/macos_triage/README.md, "Test-host prerequisites"). macOS does not
# route the whole 127.0.0.0/8 to lo0 like Linux does, and the aliases do not
# survive a reboot. This script is invoked at boot by the launchd daemon
# com.ocudu.lo0-test-aliases (RunAtLoad) and can also be run by hand:
#
#   sudo /usr/local/sbin/add_lo0_aliases.sh
#
# It is idempotent: already-present aliases are left untouched.
set -u

IFCONFIG=/sbin/ifconfig

for addr in 127.0.0.2 127.0.0.3 127.0.1.1 127.0.0.101; do
  if "$IFCONFIG" lo0 | grep -q "inet ${addr} "; then
    echo "lo0 alias ${addr} already present"
    continue
  fi
  if "$IFCONFIG" lo0 alias "$addr" up; then
    echo "lo0 alias ${addr} added"
  else
    echo "failed to add lo0 alias ${addr}" >&2
    exit 1
  fi
done
exit 0
