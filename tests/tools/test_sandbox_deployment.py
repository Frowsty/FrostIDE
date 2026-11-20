# SPDX-License-Identifier: Apache-2.0
# Source build-graph regression only: no configure, build, helper launch or native policy mutation.
from __future__ import annotations
import re
import unittest
from tools_test_support import REPO


class SandboxDeploymentTest(unittest.TestCase):
    def test_helper_refresh_is_caller_prerequisite_even_without_caller_relink(self):
        source = (REPO / "src/io/CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"function\(frost_deploy_extension_sandbox target\)(.*?)endfunction\(\)", source, re.S)
        self.assertIsNotNone(match)
        body = match.group(1)
        # A phony producer ordered after the helper runs on every caller-target build; a
        # POST_BUILD event alone never refreshes an already linked application/test image.
        self.assertIn("add_custom_target(${deployment}", body)
        self.assertIn("DEPENDS frost-extension-sandbox", body)
        self.assertIn("add_dependencies(${target} ${deployment})", body)
        self.assertIn("-E copy_if_different", body)
        self.assertIn("$<TARGET_FILE:frost-extension-sandbox> $<TARGET_FILE_DIR:${target}>", body)
        self.assertNotRegex(body, r"add_custom_command\s*\(TARGET[^)]*POST_BUILD")
        self.assertIn("if(NOT TARGET ${deployment})", body)

    def test_actual_app_and_native_probe_use_same_deployment_function(self):
        for relative, target in (("src/app/CMakeLists.txt", "frost"), ("tests/io/CMakeLists.txt", "io_integration_tests")):
            with self.subTest(target=target):
                source = (REPO / relative).read_text(encoding="utf-8")
                self.assertIn(f"frost_deploy_extension_sandbox({target})", source)


if __name__ == "__main__":
    unittest.main()
