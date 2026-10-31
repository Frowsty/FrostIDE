#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# A clean Python script for the frost-lint tests.
TEXT = "# TODO not a comment"
DOC = """
# TODO: inside a triple-quoted string
"""
print(TEXT, DOC)  # TODO(M2): a valid trailing marker
