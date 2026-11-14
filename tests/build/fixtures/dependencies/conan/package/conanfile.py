# SPDX-License-Identifier: Apache-2.0
from conan import ConanFile
from conan.tools.files import copy
import os
class FrostLocal(ConanFile):
    name = "frost-local"
    version = "1.0.0"
    package_type = "header-library"
    exports_sources = "include/*"
    def package(self):
        copy(self, "*.hpp", src=os.path.join(self.source_folder, "include"), dst=os.path.join(self.package_folder, "include"))
    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "frost-local")
        self.cpp_info.set_property("cmake_target_name", "frost-local::frost-local")
