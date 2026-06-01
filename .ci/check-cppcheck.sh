#!/usr/bin/env bash

# Run cppcheck static analysis on libx11-compat source files.
# Scope is src/*.c; tests, examples, and the generated upstream tree
# under build/upstream/ are out of scope (tests use stubs, examples are
# downstream Xlib clients, and the upstream snapshot is reviewed at
# import time).

set -e -u -o pipefail

mapfile -t SOURCES < <(git ls-files -z -- 'src/*.c' | tr '\0' '\n')

if [ ${#SOURCES[@]} -eq 0 ]; then
    echo "No tracked C source files found."
    exit 0
fi

# Run with a hard timeout so a runaway analysis cannot stall CI.
# --max-configs=1 keeps the CI invocation fast; deep analysis stays in
# the developer's hands locally.
timeout 180 cppcheck \
    -Iinclude -Isrc \
    --platform=unix64 \
    --enable=warning \
    --max-configs=1 --error-exitcode=1 --inline-suppr \
    --suppress=checkersReport --suppress=unmatchedSuppression \
    --suppress=missingIncludeSystem --suppress=noValidConfiguration \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=preprocessorErrorDirective \
    -DNARROWPROTO -DXTHREADS \
    "${SOURCES[@]}"
