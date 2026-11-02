# SPDX-License-Identifier: Apache-2.0
# Compare one Unity scratch assembly's generated graph with its compiler response file.
import argparse
import os
import re
import xml.etree.ElementTree as ET
from pathlib import Path


def unescape_msbuild(value: str) -> str:
    # MSBuild decodes each percent escape once, so escaped percent signs stay literal.
    return re.sub(r"%([0-9A-Fa-f]{2})", lambda match: chr(int(match.group(1), 16)), value)


def verify(root: Path, assembly: str, response: Path) -> None:
    unity = root / ".frost" / "unity"
    descriptor = (unity / "revision").read_text(encoding="ascii")
    if not re.fullmatch(r"1:[0-9a-f]{32}:[0-9a-f]{32}", descriptor):
        raise ValueError("invalid Unity context descriptor")
    generation = unity / "generations" / descriptor[2:34]
    projects = {p.name: ET.parse(p).getroot() for p in generation.glob("*.csproj")}
    matching = [p for p in projects.values() if unescape_msbuild(p.findtext("PropertyGroup/AssemblyName")) == assembly]
    if len(matching) != 1:
        raise ValueError("assembly project is absent or ambiguous")
    project = matching[0]
    project_names = {
        unescape_msbuild(projects[item.attrib["Include"]].findtext("PropertyGroup/AssemblyName")).casefold() + ".dll"
        for item in project.iter("ProjectReference")
    }
    def normalize(value: str) -> str:
        return os.path.normcase(os.path.normpath(os.path.join(root, value)))
    generated_items = [
        normalize(unescape_msbuild(item.findtext("HintPath")))
        for item in project.iter("Reference")
        if item.find("HintPath") is not None
    ]
    generated_items.extend(
        normalize(unescape_msbuild(unescape_msbuild(item.attrib["Include"])))
        for target in project.findall("Target") if target.get("Name") == "FrostUnityLiteralReferences"
        for item in target.iter("ReferencePath")
    )
    generated = set(generated_items)
    if len(generated) != len(generated_items):
        raise AssertionError("duplicate generated DLL reference")
    arguments = response.read_text(encoding="utf-8-sig")
    compiler_refs = re.findall(r'-r:"([^"\r\n]+)"', arguments)
    if not compiler_refs:
        raise ValueError("unsupported Unity compiler response format")
    compiler_project_names = {
        Path(path.replace("\\", "/")).name.casefold().replace(".ref.dll", ".dll")
        for path in compiler_refs
        if Path(path.replace("\\", "/")).name.casefold().replace(".ref.dll", ".dll") in project_names
    }
    compiler = {
        normalize(path) for path in compiler_refs
        if Path(path.replace("\\", "/")).name.casefold().replace(".ref.dll", ".dll") not in project_names
    }
    if generated != compiler or project_names != compiler_project_names:
        raise AssertionError(
            f"{assembly}: {len(compiler - generated)} missing and {len(generated - compiler)} extra DLL paths; "
            f"{len(project_names - compiler_project_names)} unmatched generated project references"
        )
    print(f"{assembly}: {len(generated)} exact DLL paths and {len(project_names)} project references match")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path, help="Unity scratch project root")
    parser.add_argument("assembly", help="assembly name from CompilationPipeline")
    parser.add_argument("response", type=Path, help="response file from that Unity compilation")
    args = parser.parse_args()
    verify(args.project.absolute(), args.assembly, args.response)


if __name__ == "__main__":
    main()
