#!/bin/sh

ninja -C "${BUILDDIR}"
lcov --config-file "${SRCDIR}/build-aux/.lcovrc" --directory "${BUILDDIR}" --capture --initial --output-file "${BUILDDIR}/coverage/dex.lcov"
ninja -C "${BUILDDIR}" test
lcov --config-file "${SRCDIR}/build-aux/.lcovrc" --directory "${BUILDDIR}" --capture --output-file=dex.lcov
genhtml -o "${BUILDDIR}/coverage" "${BUILDDIR}/coverage/dex.lcov"
