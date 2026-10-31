#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# A clean shell script for the frost-lint tests.
count=${#BASH_ARGV[@]}  # TODO(M1): a valid marker
echo "$count # TODO: not a comment" $#
