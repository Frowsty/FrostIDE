# SPDX-License-Identifier: Apache-2.0
# Scratch-only functional checks for the experimental MSBuild ownership probe.
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    dotnet, host = map(str, sys.argv[1:3])
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        project = root / "sample.proj"
        project.write_bytes(b'<Project><ItemGroup><ClCompile Include="a.cpp"/>'
                            b'<ClCompile Include="b.cpp"/></ItemGroup></Project>')

        def request(remove=None):
            command = {"Project": str(project), "Configuration": "Debug", "Platform": "x64", "Remove": remove}
            result = subprocess.run([dotnet, host], input=json.dumps(command) + "\n", text=True,
                                    capture_output=True, check=True, env=os.environ.copy())
            return json.loads(result.stdout)

        original = project.read_bytes()
        result = request("a.cpp")
        assert result["Error"] is None, result
        plan = result["Edit"]
        assert plan["Sha256"] == hashlib.sha256(original).hexdigest().upper()
        assert project.read_bytes() == original
        edited = original[:plan["Offset"]] + original[plan["Offset"] + plan["Length"]:]
        project.write_bytes(edited)  # Scratch fixture only; this is not a commit protocol.
        assert [i["EvaluatedInclude"] for i in request()["Items"]] == ["b.cpp"]
        assert request("a.cpp")["Error"]

        project.write_bytes(b'<Project><ItemGroup><ClCompile Include="*.cpp"/></ItemGroup></Project>')
        (root / "a.cpp").touch()
        assert request("a.cpp")["Error"]
        project.write_bytes(b'<Project><ItemGroup Condition="true"><ClCompile Include="a.cpp"/>'
                            b'</ItemGroup></Project>')
        assert request("a.cpp")["Error"]
        project.write_bytes(b'<Project><!-- <ClCompile Include="a.cpp"/> --><ItemGroup>'
                            b'<ClCompile Include="a.cpp" Extra="x"/></ItemGroup></Project>')
        assert request("a.cpp")["Error"]
        print("ownership and edit-plan checks passed")


if __name__ == "__main__":
    main()
