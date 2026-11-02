# SPDX-License-Identifier: Apache-2.0
# Verify generated Unity project literals through actual MSBuild evaluation.
import argparse
import json
import os
import runpy
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("dotnet", type=Path)
    parser.add_argument("profile", type=Path)
    args = parser.parse_args()
    if not all(path.is_absolute() for path in (args.project, args.dotnet, args.profile)):
        raise ValueError("all paths must be absolute")
    runpy.run_path(str(Path(__file__).with_name("managed_semantics_probe.py")))["require_existing_windows_migration_marker"]()
    unity = args.project / ".frost" / "unity"
    descriptor = (unity / "revision").read_text(encoding="ascii")
    import re
    if not re.fullmatch(r"1:[0-9a-f]{32}:[0-9a-f]{32}", descriptor):
        raise ValueError("invalid generated Unity descriptor")
    generation = unity / "generations" / descriptor[2:34]
    projects = [p for p in generation.glob("*.csproj")
                if ET.parse(p).getroot().findtext("PropertyGroup/AssemblyName") == "Frost.Unity.Probe"]
    if len(projects) != 1:
        raise ValueError("scratch assembly project is missing or ambiguous")
    env = os.environ.copy()
    for name in ("LOCALAPPDATA", "APPDATA", "HOME", "USERPROFILE", "XDG_DATA_HOME", "XDG_CONFIG_HOME",
                 "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH", "DOTNET_CLI_HOME"):
        path = args.profile / name
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    env.update(DOTNET_ROOT=str(args.dotnet.parent), DOTNET_NOLOGO="1", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
               DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_GENERATE_ASPNET_CERTIFICATE="0",
               DOTNET_ADD_GLOBAL_TOOLS_TO_PATH="0")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    result = subprocess.run([str(args.dotnet), "msbuild", str(projects[0]), "-getItem:Compile,Reference", "-nologo"],
                            cwd=args.project, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace", creationflags=flags, timeout=30)
    if result.returncode:
        raise RuntimeError("MSBuild evaluation failed: " + result.stderr[-1500:])
    items = json.loads(result.stdout)["Items"]
    source = args.project / "Assets" / "Probe" / "Frost;literal%20$(Property).cs"
    reference = args.project / "Assets" / "Plugins" / "Frost;literal%20$(Property).dll"
    same = lambda a, b: os.path.normcase(a) == os.path.normcase(str(b))
    if sum(same(item["FullPath"], source) for item in items["Compile"]) != 1:
        raise AssertionError("MSBuild did not preserve exactly one literal source path")
    import re
    decode = lambda value: re.sub(r"%([0-9A-Fa-f]{2})", lambda match: chr(int(match.group(1), 16)), value)
    root = ET.parse(projects[0]).getroot()
    literal = [decode(decode(item.attrib["Include"])) for target in root.findall("Target")
               if target.get("Name") == "FrostUnityLiteralReferences" for item in target.iter("ReferencePath")]
    if sum(same(path, reference) for path in literal) != 1:
        raise AssertionError("generated target did not preserve the literal reference")
    if any(same(item.get("HintPath", ""), reference) for item in items["Reference"]):
        raise AssertionError("literal reference was duplicated")
    # CscCommandLineArgs item display is itself unescaped; execute the compiler to
    # distinguish the real source paths instead of interpreting that display.
    fixture = args.profile / "actual-compiler"
    fixture.mkdir(parents=True, exist_ok=True)
    names = ("Frost;literal%20$(Property).cs", "Frost;literal $(Property).cs", "Use.cs")
    for name, content in zip(names, ("public class LiteralPercent {}", "public class LiteralSpace {}",
                                     "public class Use { public LiteralPercent A; public LiteralSpace B; }")):
        (fixture / name).write_text(content, encoding="utf-8")
    escape = lambda value: "".join(f"%{ord(c):02X}" if c in "%$@'();?*" else c for c in value)
    project = ET.Element("Project", Sdk="Microsoft.NET.Sdk")
    properties = ET.SubElement(project, "PropertyGroup")
    ET.SubElement(properties, "TargetFramework").text = "net10.0"
    ET.SubElement(properties, "EnableDefaultCompileItems").text = "false"
    group = ET.SubElement(project, "ItemGroup")
    for name in names:
        ET.SubElement(group, "Compile", Include=escape(name))
    ET.ElementTree(project).write(fixture / "probe.csproj", encoding="utf-8")
    config = fixture / "NuGet.Config"
    config.write_text("<configuration><packageSources><clear /></packageSources></configuration>", encoding="utf-8")
    compile_result = subprocess.run([str(args.dotnet), "build", str(fixture / "probe.csproj"), "--configfile",
                                     str(config), "-m:1", "--nologo", "-p:UseSharedCompilation=false"],
                                    cwd=fixture, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, encoding="utf-8", errors="replace", creationflags=flags, timeout=180)
    if compile_result.returncode:
        raise RuntimeError("actual compiler did not preserve distinct source paths: " + compile_result.stdout[-3000:])
    print("actual compiler resolved distinct literal-percent and space source files")

    print("literal semicolon, percent escape and MSBuild expression paths survived evaluation")


if __name__ == "__main__":
    main()
