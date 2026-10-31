#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# licensecheck.py: frost-licensecheck v0, the dependency licence gate and SBOM writer
# (PLAN.md section 2 "Licences" and "GUI toolkit", section 6.3 "Checks").
#
#   python tools/licensecheck.py [--repo <path>] [--vcpkg-installed <dir>]...
#                                [--build-dir <dir>]... [--qt-root <dir>] [--sbom <file>]
#
# Checks:
#   vcpkg ports  every port in each installed tree (default: every <repo>/build/vcpkg_installed*
#                directory, the VCPKG_INSTALLED_DIRs of CMakePresets.json) must declare a licence
#                expression the SPDX allowlist satisfies, or have an ADR-approved
#                exception (ADR_EXCEPTIONS); anything else "needs an ADR".
#   Qt modules   the modules and plugins the build uses, derived from find_package(Qt6 ...)
#                and Qt6:: targets in the repository's CMake files and from the Qt
#                libraries next to (or, on Linux, DT_NEEDED by) built binaries, must be
#                available under LGPL-3.0-only (GPL-only modules such as Charts and Canvas
#                Painter fail) and must be on the per-platform module allowlist for their
#                context (runtime, icon baker, tests).
# Module licences come from Qt's own SBOMs (<qt-root>/sbom/*.spdx.json, qt-root defaulting
# to $FROST_QT_ROOT) and fall back to QT_MODULE_LICENCES for modules not installed.
#
# --sbom <file> writes an SPDX 2.3 JSON SBOM of FrostIDE, the vcpkg ports and the Qt
# components with their declared licences (SOURCE_DATE_EPOCH fixes the timestamp).
#
# Output: one "path:line: message" line per violation on stdout; exit status 1 on any
# violation, 0 otherwise, 2 on usage errors. Python 3.11+, standard library only.
#
# TODO(M1): ScanCode Toolkit licence-notice detection on diffs (PLAN.md 6.7, clean room);
# it is a new third-party tool and needs its own ADR and licence check first.
# TODO(M1): add the bundled-process class (appimageupdatetool) and the vsdbg/cpptools
# artifact scan (PLAN.md 2 "Licences" and 6.3 "Checks").

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import struct
import sys
import uuid
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frost_lint  # noqa: E402  (shared repository file listing and CMake lexer)

# ---------------------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------------------

# The SPDX allowlist for linked code, from PLAN.md section 2 "Licences". Each entry is a
# simple expression (a licence ID, or "ID WITH exception") matched exactly, so
# "Apache-2.0 WITH LLVM-exception" never stands in for another exception and
# "GPL-3.0-or-later" never matches anything. Compound expressions are evaluated: OR needs
# one allowed alternative (the dependency may be used under it), AND needs all of them.
ALLOWLIST = frozenset({
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSD-3-Clause WITH PCRE2-exception",
    "Apache-2.0",
    "Apache-2.0 WITH LLVM-exception",
    "BSL-1.0",
    "Zlib",
    "ISC",
    "curl",
    "blessing",
    "OFL-1.1",
})
# LGPL-3.0-only is allowed only for Qt, which is linked dynamically (PLAN.md 2 "GUI toolkit").
QT_ALLOWLIST = ALLOWLIST | {"LGPL-3.0-only"}

# vcpkg ports whose licence is not satisfied by ALLOWLIST but which an accepted ADR
# approves: port name -> (the exact declared expression, the ADR path). An upgrade that
# changes the expression fails again until the ADR is revisited. Empty in M0.
ADR_EXCEPTIONS: dict[str, tuple[str, str]] = {}

# Qt modules allowed at runtime, per platform (PLAN.md 2 "GUI toolkit"): Core, Gui,
# Widgets and the platform plugin; Linux adds DBus, XcbQpa, WaylandClient and the IBus
# input-context plugin. EntryPoint is Qt's static WinMain shim (BSD-3-Clause), linked
# implicitly into Windows GUI executables.
QT_RUNTIME_MODULES = {
    "windows": frozenset({"Core", "Gui", "Widgets", "EntryPoint"}),
    "linux": frozenset({"Core", "Gui", "Widgets", "DBus", "XcbQpa", "WaylandClient"}),
}
# Plugins allowed at runtime, as "<plugin dir>/<file stem>". The offscreen platform
# plugin ships next to frost because `frost --selftest` uses it (PLAN.md M0).
QT_RUNTIME_PLUGINS = {
    "windows": frozenset({"platforms/qwindows", "platforms/qoffscreen"}),
    "linux": frozenset({"platforms/qxcb", "platforms/qwayland", "platforms/qoffscreen",
                        "platforminputcontexts/ibusplatforminputcontextplugin"}),
}
# Extra modules per context: Svg only in the icon baker, Test only in tests.
QT_CONTEXT_MODULES = {"runtime": frozenset(), "icon-baker": frozenset({"Svg"}), "test": frozenset({"Test"})}

_QT_LGPL = "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
_QT_GPL = "LicenseRef-Qt-Commercial OR GPL-3.0-only"
# Licences of Qt modules, used when the installed Qt has no SBOM entry for a module (for
# example a module that is not installed at all). The LGPL entries are the declarations
# in the Qt 6.12.0 SBOMs (qtbase, qtsvg, qttools). The GPL-only entries are modules whose
# documentation says "available under commercial licenses ... In addition, it is available
# under the GNU General Public License, version 3" with no LGPL option (doc.qt.io/qt-6,
# the module index pages, checked 2026-10-31).
QT_MODULE_LICENCES: dict[str, str] = {
    **{m: _QT_LGPL for m in (
        "Core", "Gui", "Widgets", "DBus", "Network", "Concurrent", "Sql", "Xml", "OpenGL",
        "OpenGLWidgets", "PrintSupport", "Test", "XcbQpa", "WaylandClient", "WlShellIntegration",
        "Svg", "SvgWidgets", "UiTools", "UiPlugin", "Help", "Designer", "Linguist", "Tools")},
    "EntryPoint": "LicenseRef-Qt-Commercial OR BSD-3-Clause",
    **{m: _QT_GPL for m in (
        "Charts", "DataVisualization", "Graphs", "CanvasPainter", "NetworkAuth", "HttpServer",
        "VirtualKeyboard", "Quick3D", "Quick3DPhysics", "QuickTimeline", "Lottie", "Bodymovin",
        "Coap", "Mqtt")},
}
# Plugin file stems and CMake plugin targets -> the plugin's SBOM target name.
QT_PLUGIN_TARGETS: dict[str, str] = {
    "platforms/qwindows": "QWindowsIntegrationPlugin",
    "platforms/qoffscreen": "QOffscreenIntegrationPlugin",
    "platforms/qminimal": "QMinimalIntegrationPlugin",
    "platforms/qdirect2d": "QWindowsDirect2DIntegrationPlugin",
    "platforms/qxcb": "QXcbIntegrationPlugin",
    "platforms/qwayland": "QWaylandIntegrationPlugin",
    "platforminputcontexts/ibusplatforminputcontextplugin": "QIbusPlatformInputContextPlugin",
    "platforminputcontexts/composeplatforminputcontextplugin": "QComposePlatformInputContextPlugin",
    "styles/qmodernwindowsstyle": "QModernWindowsStylePlugin",
}
_PLUGIN_BY_TARGET = {target: key for key, target in QT_PLUGIN_TARGETS.items()}
# Licence of plugins absent from the installed SBOMs: all of the above live in qtbase.
_QT_PLUGIN_FALLBACK = _QT_LGPL
# Directories under a Qt plugin root that hold plugins the binary scan recognises.
_PLUGIN_DIRS = ("platforms", "platforminputcontexts", "styles", "imageformats", "iconengines",
                "tls", "networkinformation", "sqldrivers", "generic", "platformthemes",
                "wayland-shell-integration", "wayland-decoration-client")

# ---------------------------------------------------------------------------------------
# SPDX expressions
# ---------------------------------------------------------------------------------------

_SPDX_TOKEN = re.compile(r"\s*(\(|\)|[A-Za-z0-9.+\-:]+)")


class ExpressionError(ValueError):
    """Raised for an SPDX expression that does not parse."""


def parse_expression(text: str):
    """
    Parses an SPDX license expression (SPDX 2.3 Annex D) into nested tuples:
    ("id", "MIT"), ("with", "Apache-2.0", "LLVM-exception"), ("and", [...]), ("or", [...]).
    AND binds tighter than OR; operators are upper case.
    """
    tokens, pos = [], 0
    while pos < len(text):
        m = _SPDX_TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip() == "":
                break
            raise ExpressionError(f"unexpected character in {text!r}")
        tokens.append(m.group(1))
        pos = m.end()
    if not tokens:
        raise ExpressionError("empty expression")
    index = 0

    def peek():
        return tokens[index] if index < len(tokens) else None

    def take():
        nonlocal index
        index += 1
        return tokens[index - 1]

    def primary():
        token = peek()
        if token is None or token in (")", "AND", "OR", "WITH"):
            raise ExpressionError(f"expected a licence in {text!r}")
        take()
        if token == "(":
            node = disjunction()
            if peek() != ")":
                raise ExpressionError(f"missing ')' in {text!r}")
            take()
            return node
        if peek() == "WITH":
            take()
            exception = peek()
            if exception is None or exception in ("(", ")", "AND", "OR", "WITH"):
                raise ExpressionError(f"expected an exception after WITH in {text!r}")
            take()
            return ("with", token, exception)
        return ("id", token)

    def conjunction():
        nodes = [primary()]
        while peek() == "AND":
            take()
            nodes.append(primary())
        return nodes[0] if len(nodes) == 1 else ("and", nodes)

    def disjunction():
        nodes = [conjunction()]
        while peek() == "OR":
            take()
            nodes.append(conjunction())
        return nodes[0] if len(nodes) == 1 else ("or", nodes)

    node = disjunction()
    if index != len(tokens):
        raise ExpressionError(f"unexpected {tokens[index]!r} in {text!r}")
    return node


def expression_allowed(text: str, allowed: frozenset[str]) -> bool:
    """True when `text` can be satisfied using only simple expressions from `allowed`."""

    def ok(node) -> bool:
        kind = node[0]
        if kind == "id":
            return node[1] in allowed
        if kind == "with":
            return f"{node[1]} WITH {node[2]}" in allowed
        if kind == "and":
            return all(ok(child) for child in node[1])
        return any(ok(child) for child in node[1])

    return ok(parse_expression(text))


def _licence_refs(text: str) -> set[str]:
    return set(re.findall(r"LicenseRef-[A-Za-z0-9.\-]+", text))


# ---------------------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


@dataclass
class Package:
    """One SBOM package: a vcpkg port or a Qt component."""

    spdx_id: str
    name: str
    version: str
    declared: str
    concluded: str
    download: str = "NOASSERTION"
    homepage: str | None = None
    purl: str | None = None
    comment: str = ""
    relationship: str = "DEPENDS_ON"


@dataclass
class Report:
    violations: list[Violation] = field(default_factory=list)
    packages: list[Package] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


def _display(repo: Path, path: Path) -> str:
    """`path` relative to the repository when inside it, POSIX separators."""
    try:
        return path.resolve().relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix()


def _spdx_id(text: str) -> str:
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.\-]+", "-", text).strip("-")


# ---------------------------------------------------------------------------------------
# vcpkg ports
# ---------------------------------------------------------------------------------------


def _installed_ports(root: Path, triplet: str) -> list[str]:
    """Ports installed for `triplet`, from vcpkg's status database (else the share dirs)."""
    status = root / "vcpkg" / "status"
    if status.is_file():
        ports = []
        for paragraph in status.read_text(encoding="utf-8").split("\n\n"):
            fields = dict(
                line.split(": ", 1) for line in paragraph.splitlines() if ": " in line and not line.startswith(" ")
            )
            if (fields.get("Architecture") == triplet and "Feature" not in fields
                    and fields.get("Status", "").endswith(" installed") and "Package" in fields):
                ports.append(fields["Package"])
        return sorted(set(ports))
    share = root / triplet / "share"
    return sorted(p.name for p in share.iterdir() if (p / "vcpkg.spdx.json").is_file())


def _port_licence(document: dict) -> tuple[str | None, dict]:
    """The port package of a vcpkg.spdx.json and its licence expression (or None)."""
    packages = document.get("packages", [])
    port = next((p for p in packages if p.get("SPDXID") == "SPDXRef-port"), packages[0] if packages else {})
    # vcpkg writes the port's "license" field to licenseConcluded and leaves licenseDeclared
    # as NOASSERTION; prefer licenseDeclared when a future vcpkg fills it in.
    for key in ("licenseDeclared", "licenseConcluded"):
        value = port.get(key)
        if value and value not in ("NOASSERTION", "NONE"):
            return value, port
    return None, port


def check_vcpkg(repo: Path, roots: list[Path], report: Report) -> None:
    """Checks every installed vcpkg port's declared licence against the allowlist."""
    trees = []
    for root in roots:
        if root.is_dir():
            trees += [(root, d.name) for d in sorted(root.iterdir())
                      if d.is_dir() and d.name != "vcpkg" and (d / "share").is_dir()]
    if not trees:
        manifest = repo / "vcpkg.json"
        try:
            needs = bool(json.loads(manifest.read_text(encoding="utf-8")).get("dependencies"))
        except (OSError, ValueError):
            needs = False
        if needs:
            where = ", ".join(_display(repo, r) for r in roots)
            report.violations.append(Violation(
                "vcpkg.json", 1,
                f"no vcpkg installed tree found ({where}); configure a build first or pass --vcpkg-installed"))
        return
    for root, triplet in trees:
        for port in _installed_ports(root, triplet):
            spdx_path = root / triplet / "share" / port / "vcpkg.spdx.json"
            shown = _display(repo, spdx_path)
            try:
                document = json.loads(spdx_path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                report.violations.append(Violation(shown, 1, f"vcpkg port {port}: cannot read its SPDX file ({error})"))
                continue
            expression, info = _port_licence(document)
            version = info.get("versionInfo", "NOASSERTION")
            purl = next((r.get("referenceLocator") for r in info.get("externalRefs", [])
                         if r.get("referenceType") == "purl"), None)
            report.packages.append(Package(
                _spdx_id(f"vcpkg-{triplet}-{port}"), port, version, expression or "NOASSERTION", "NOASSERTION",
                info.get("downloadLocation", "NOASSERTION"), info.get("homepage"), purl,
                f"vcpkg port, triplet {triplet}"))
            if expression is None:
                report.violations.append(Violation(
                    shown, 1, f"vcpkg port {port} {version} declares no licence; needs an ADR"))
                continue
            exception = ADR_EXCEPTIONS.get(port)
            if exception and exception[0] == expression:
                report.notes.append(f"vcpkg port {port}: '{expression}' approved by {exception[1]}")
                continue
            try:
                allowed = expression_allowed(expression, ALLOWLIST)
            except ExpressionError as error:
                report.violations.append(Violation(shown, 1, f"vcpkg port {port}: {error}; needs an ADR"))
                continue
            if not allowed:
                report.violations.append(Violation(
                    shown, 1,
                    f"vcpkg port {port} {version} declares '{expression}', which the SPDX allowlist does not "
                    f"satisfy; needs an ADR (then record it in ADR_EXCEPTIONS in tools/licensecheck.py)"))


# ---------------------------------------------------------------------------------------
# Qt usage
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class QtUse:
    """One use of a Qt component: a module name or a plugin key ("platforms/qwindows")."""

    component: str
    is_plugin: bool
    context: str  # "runtime", "icon-baker" or "test"
    platforms: tuple[str, ...]  # platforms whose allowlist applies
    path: str
    line: int


def context_for(rel_path: str) -> str:
    """The Qt usage context of a repository or build-tree path."""
    lowered = "/" + rel_path.lower()
    if "/tests/" in lowered:
        return "test"
    if re.search(r"icon[-_]?bak", lowered):
        return "icon-baker"
    return "runtime"


_FIND_QT = re.compile(r"\bfind_package\s*\(\s*Qt6?(\w*)\b([^)]*)\)", re.IGNORECASE)
_QT_TARGET = re.compile(r"\bQt6?::(\w+)")
_KEYWORD = re.compile(r"^[A-Z][A-Z0-9_]+$")


def _module_from_target(name: str) -> tuple[str, bool] | None:
    """Maps a Qt6:: target to (module or plugin key, is_plugin); None for build tools."""
    if not name or name[0].islower():
        return None  # Qt6::qmake, Qt6::moc, Qt6::windeployqt...: tools, never linked
    if name.endswith("Plugin"):
        return _PLUGIN_BY_TARGET.get(name, "plugin:" + name), True
    if name.endswith("Private"):
        name = name[: -len("Private")]
    return name, False


def cmake_qt_uses(repo: Path, files: list[str]) -> list[QtUse]:
    """Qt components named in the repository's CMake files (fixtures and goldens excluded)."""
    uses = []
    every_platform = tuple(sorted(QT_RUNTIME_MODULES))
    for rel in files:
        name = rel.rsplit("/", 1)[-1]
        if frost_lint.is_data_path(rel) or not (name == "CMakeLists.txt" or name.endswith((".cmake", ".cmake.in"))):
            continue
        try:
            text = (repo / rel).read_text(encoding="utf-8").replace("\r\n", "\n")
        except (OSError, UnicodeDecodeError):
            continue
        code = frost_lint.scan(text, frost_lint.CMAKE).code
        lines = frost_lint._Lines(code)
        context = context_for(rel)
        for m in _FIND_QT.finditer(code):
            line = lines.line(m.start())
            if m.group(1):
                uses.append(QtUse(m.group(1), False, context, every_platform, rel, line))
                continue
            collecting = False
            for token in m.group(2).split():
                if token in ("COMPONENTS", "OPTIONAL_COMPONENTS", "REQUIRED"):
                    collecting = True
                elif _KEYWORD.match(token) or "${" in token or token[0].isdigit():
                    collecting = collecting and token[0].isdigit()
                elif collecting:
                    uses.append(QtUse(token, False, context, every_platform, rel, line))
        for m in _QT_TARGET.finditer(code):
            mapped = _module_from_target(m.group(1))
            if mapped:
                uses.append(QtUse(mapped[0], mapped[1], context, every_platform, rel, lines.line(m.start())))
    return uses


_QT_DLL = re.compile(r"^Qt6(\w+)\.dll$", re.IGNORECASE)
_QT_SO = re.compile(r"^libQt6(\w+)\.so(?:\.\d+)*$")
_SKIP_BUILD_DIRS = {"CMakeFiles", "vcpkg_installed", ".cmake", "Testing", "sbom", "_deps"}


def _module_from_library(stem: str) -> str:
    """Qt6Widgetsd -> Widgets: drops the Windows debug suffix unless the name is a module."""
    if stem in QT_MODULE_LICENCES or not stem.endswith("d"):
        return stem
    return stem[:-1] if stem[:-1] in QT_MODULE_LICENCES else stem


def elf_needed(path: Path) -> list[str]:
    """
    DT_NEEDED entries of a 64-bit little-endian ELF executable or shared object, read from
    the section headers (the SHT_DYNAMIC section and its linked string table). Returns []
    for anything else, including relocatable objects. Written from the ELF specification.
    """
    try:
        with path.open("rb") as stream:
            header = stream.read(64)
            if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] != 2 or header[5] != 1:
                return []
            e_type = struct.unpack_from("<H", header, 16)[0]
            if e_type not in (2, 3):  # ET_EXEC, ET_DYN
                return []
            shoff = struct.unpack_from("<Q", header, 40)[0]
            shentsize, shnum = struct.unpack_from("<HH", header, 58)
            if not shoff or shentsize < 64 or shnum == 0 or shnum > 4096:
                return []
            stream.seek(shoff)
            table = stream.read(shentsize * shnum)
            sections = [struct.unpack_from("<IIQQQQIIQQ", table, i * shentsize) for i in range(shnum)]
            needed = []
            for _, sh_type, _, _, offset, size, link, _, _, entsize in sections:
                if sh_type != 6 or link >= shnum:  # SHT_DYNAMIC
                    continue
                str_offset, str_size = sections[link][4], sections[link][5]
                stream.seek(str_offset)
                strings = stream.read(str_size)
                stream.seek(offset)
                dynamic = stream.read(size)
                step = entsize or 16
                for k in range(0, len(dynamic) - 15, step):
                    tag, value = struct.unpack_from("<qQ", dynamic, k)
                    if tag == 0:
                        break
                    if tag == 1 and value < len(strings):  # DT_NEEDED
                        needed.append(strings[value: strings.find(b"\0", value)].decode("utf-8", "replace"))
            return needed
    except (OSError, struct.error):
        return []


def binary_qt_uses(repo: Path, build_dirs: list[Path]) -> list[QtUse]:
    """Qt libraries and plugins found next to (or needed by) binaries in the build trees."""
    uses = []
    for build in build_dirs:
        if not build.is_dir():
            continue
        for directory, subdirs, names in os.walk(build):
            subdirs[:] = [d for d in subdirs if d not in _SKIP_BUILD_DIRS]
            folder = Path(directory)
            rel_dir = folder.relative_to(build).as_posix()
            context = context_for(rel_dir + "/")
            parent = folder.name
            for name in names:
                path = folder / name
                shown = _display(repo, path)
                lower = name.lower()
                m = _QT_DLL.match(name)
                if m:
                    uses.append(QtUse(_module_from_library(m.group(1)), False, context, ("windows",), shown, 1))
                    continue
                m = _QT_SO.match(name)
                if m:
                    uses.append(QtUse(m.group(1), False, context, ("linux",), shown, 1))
                    continue
                if parent in _PLUGIN_DIRS and (lower.endswith(".dll") or ".so" in lower):
                    platform = "windows" if lower.endswith(".dll") else "linux"
                    stem = name.split(".", 1)[0]
                    if platform == "linux" and stem.startswith("lib"):
                        stem = stem[3:]
                    key = f"{parent}/{stem}"
                    if platform == "windows" and key not in QT_PLUGIN_TARGETS and key[:-1] in QT_PLUGIN_TARGETS:
                        key = key[:-1]  # debug plugin, e.g. qwindowsd.dll
                    uses.append(QtUse(key, True, context, (platform,), shown, 1))
                    continue
                if lower.endswith((".o", ".a", ".obj", ".lib", ".pdb", ".ninja", ".txt", ".cmake", ".json")):
                    continue
                for needed in elf_needed(path):
                    nm = _QT_SO.match(needed)
                    if nm:
                        uses.append(QtUse(nm.group(1), False, context, ("linux",), shown, 1))
    return uses


def load_qt_sbom(qt_root: Path | None) -> dict[tuple[str, str], str]:
    """{("module"|"plugin", name): declared licence} from Qt's SBOMs under <qt_root>/sbom."""
    licences: dict[tuple[str, str], str] = {}
    if qt_root is None or not (qt_root / "sbom").is_dir():
        return licences
    pattern = re.compile(r"-qt-(module|plugin)-(.+)-[0-9a-f]{6,}$")
    for path in sorted((qt_root / "sbom").glob("*.spdx.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for package in document.get("packages", []):
            m = pattern.search(package.get("SPDXID", ""))
            expression = package.get("licenseDeclared") or package.get("licenseConcluded")
            if not m or not expression or expression == "NOASSERTION":
                continue
            kind, name = m.group(1), m.group(2)
            if kind == "module" and name.endswith("Private"):
                name = name[: -len("Private")]
            licences.setdefault((kind, name), expression)
    return licences


def qt_version(qt_root: Path | None) -> str:
    if qt_root is not None:
        for path in sorted((qt_root / "sbom").glob("qtbase-*.spdx.json")) if (qt_root / "sbom").is_dir() else []:
            m = re.match(r"qtbase-(.+)\.spdx\.json$", path.name)
            if m:
                return m.group(1)
    return "NOASSERTION"


def check_qt(repo: Path, uses: list[QtUse], qt_root: Path | None, report: Report) -> None:
    """Checks each used Qt component's licence and its place on the module allowlist."""
    sbom = load_qt_sbom(qt_root)
    version = qt_version(qt_root)
    seen_packages: set[str] = set()
    for use in sorted(set(uses)):
        if use.is_plugin:
            target = QT_PLUGIN_TARGETS.get(use.component, use.component.removeprefix("plugin:"))
            licence = sbom.get(("plugin", target))
            if licence is None and use.component in QT_PLUGIN_TARGETS:
                licence = _QT_PLUGIN_FALLBACK
            label = f"Qt plugin {use.component}"
        else:
            licence = sbom.get(("module", use.component)) or QT_MODULE_LICENCES.get(use.component)
            label = f"Qt module {use.component}"
        try:
            lgpl = licence is not None and expression_allowed(licence, QT_ALLOWLIST)
        except ExpressionError as error:
            report.violations.append(Violation(use.path, use.line, f"{label}: {error}; needs an ADR"))
            continue
        if use.component not in seen_packages:
            seen_packages.add(use.component)
            report.packages.append(Package(
                _spdx_id(f"Qt-{use.component}"), f"Qt {use.component}", version, licence or "NOASSERTION",
                "LGPL-3.0-only" if lgpl else "NOASSERTION",
                "https://download.qt.io/", "https://www.qt.io/", None,
                "Qt plugin" if use.is_plugin else "Qt module, linked dynamically",
                "TEST_DEPENDENCY_OF" if use.context == "test" else "DEPENDS_ON"))
        if licence is None:
            report.violations.append(Violation(
                use.path, use.line,
                f"{label} has no known licence (not in the Qt SBOMs or QT_MODULE_LICENCES); needs an ADR"))
            continue
        if not lgpl:
            report.violations.append(Violation(
                use.path, use.line,
                f"{label} is licensed '{licence}' with no LGPL-3.0-only option (GPL-only); FrostIDE links only "
                f"LGPL-3.0-only Qt modules (PLAN.md 2, GUI toolkit)"))
            continue
        allowed_any = False
        for platform in use.platforms:
            if use.is_plugin:
                allowed = QT_RUNTIME_PLUGINS[platform]
            else:
                allowed = QT_RUNTIME_MODULES[platform] | QT_CONTEXT_MODULES.get(use.context, frozenset())
            allowed_any = allowed_any or use.component in allowed
        if not allowed_any:
            where = "/".join(use.platforms)
            report.violations.append(Violation(
                use.path, use.line,
                f"{label} is not on the {where} {use.context} allowlist (PLAN.md 2, GUI toolkit: Core, Gui, "
                f"Widgets and the platform plugin; Linux adds DBus, XcbQpa, WaylandClient and the IBus plugin; "
                f"Svg only in the icon baker, Test only in tests); changing it needs an ADR"))


# ---------------------------------------------------------------------------------------
# SBOM
# ---------------------------------------------------------------------------------------


def write_sbom(repo: Path, report: Report, path: Path) -> None:
    """Writes an SPDX 2.3 JSON document describing FrostIDE and its dependencies."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    moment = (datetime.datetime.fromtimestamp(int(epoch), datetime.UTC) if epoch and epoch.isdigit()
              else datetime.datetime.now(datetime.UTC))
    try:
        version = json.loads((repo / "vcpkg.json").read_text(encoding="utf-8")).get("version", "NOASSERTION")
    except (OSError, ValueError):
        version = "NOASSERTION"
    root = {
        "SPDXID": "SPDXRef-FrostIDE", "name": "FrostIDE", "versionInfo": version,
        "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
        "licenseConcluded": "Apache-2.0", "licenseDeclared": "Apache-2.0",
        "copyrightText": "Copyright 2026 The FrostIDE authors",
        "primaryPackagePurpose": "APPLICATION",
    }
    packages, relationships, used_ids = [root], [], {"SPDXRef-FrostIDE"}
    relationships.append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                          "relatedSpdxElement": "SPDXRef-FrostIDE"})
    refs: set[str] = set()
    for package in report.packages:
        spdx_id, suffix = package.spdx_id, 2
        while spdx_id in used_ids:
            spdx_id, suffix = f"{package.spdx_id}-{suffix}", suffix + 1
        used_ids.add(spdx_id)
        entry = {
            "SPDXID": spdx_id, "name": package.name, "versionInfo": package.version,
            "downloadLocation": package.download or "NOASSERTION", "filesAnalyzed": False,
            "licenseConcluded": package.concluded, "licenseDeclared": package.declared,
            "copyrightText": "NOASSERTION", "comment": package.comment,
        }
        if package.homepage:
            entry["homepage"] = package.homepage
        if package.purl:
            entry["externalRefs"] = [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                      "referenceLocator": package.purl}]
        packages.append(entry)
        refs |= _licence_refs(package.declared) | _licence_refs(package.concluded)
        if package.relationship == "TEST_DEPENDENCY_OF":
            relationships.append({"spdxElementId": spdx_id, "relationshipType": "TEST_DEPENDENCY_OF",
                                  "relatedSpdxElement": "SPDXRef-FrostIDE"})
        else:
            relationships.append({"spdxElementId": "SPDXRef-FrostIDE", "relationshipType": "DEPENDS_ON",
                                  "relatedSpdxElement": spdx_id})
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"FrostIDE-{version}",
        "documentNamespace": f"https://frostide.invalid/spdx/FrostIDE-{version}-{uuid.uuid4()}",
        "creationInfo": {"created": moment.strftime("%Y-%m-%dT%H:%M:%SZ"),
                         "creators": ["Tool: frost-licensecheck-0"]},
        "packages": packages,
        "relationships": relationships,
    }
    if refs:
        document["hasExtractedLicensingInfos"] = [
            {"licenseId": ref, "name": ref.removeprefix("LicenseRef-"),
             "extractedText": ("A licence that is not on the SPDX list, named by the upstream SBOM (for Qt, The "
                               "Qt Company's commercial licence). FrostIDE does not use it.")}
            for ref in sorted(refs)
        ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------


def run(repo: Path, vcpkg_roots: list[Path], build_dirs: list[Path], qt_root: Path | None) -> Report:
    """Runs every check and returns the violations and SBOM packages."""
    report = Report()
    check_vcpkg(repo, vcpkg_roots, report)
    files = frost_lint.list_repo_files(repo)
    uses = cmake_qt_uses(repo, files) + binary_qt_uses(repo, build_dirs)
    check_qt(repo, uses, qt_root, report)
    report.violations = sorted(set(report.violations))
    return report


def default_vcpkg_roots(repo: Path) -> list[Path]:
    """Every <repo>/build/vcpkg_installed* tree (the presets give the ASan triplet its
    own); <repo>/build/vcpkg_installed when there is none, so a missing tree is reported."""
    build = repo / "build"
    roots = sorted(d for d in build.glob("vcpkg_installed*") if d.is_dir()) if build.is_dir() else []
    return roots or [build / "vcpkg_installed"]


def default_build_dirs(repo: Path) -> list[Path]:
    build = repo / "build"
    if not build.is_dir():
        return []
    return [d for d in sorted(build.iterdir())
            if d.is_dir() and not d.name.startswith("vcpkg_installed") and d.name != "sbom"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="frost-licensecheck v0: dependency licences and the SBOM")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("--vcpkg-installed", type=Path, action="append", default=None,
                        help="vcpkg installed root (repeatable; default every <repo>/build/vcpkg_installed*)")
    parser.add_argument("--build-dir", type=Path, action="append", default=None,
                        help="build tree to scan for Qt libraries (repeatable; default every <repo>/build/*)")
    parser.add_argument("--qt-root", type=Path, default=None,
                        help="Qt prefix whose sbom/ directory supplies module licences (default $FROST_QT_ROOT)")
    parser.add_argument("--sbom", type=Path, default=None, help="write an SPDX 2.3 JSON SBOM to this file")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    if not repo.is_dir():
        print(f"licensecheck: {repo} is not a directory", file=sys.stderr)
        return 2
    vcpkg_roots = args.vcpkg_installed or default_vcpkg_roots(repo)
    build_dirs = args.build_dir if args.build_dir is not None else default_build_dirs(repo)
    qt_root = args.qt_root
    if qt_root is None and os.environ.get("FROST_QT_ROOT"):
        qt_root = Path(os.environ["FROST_QT_ROOT"])
    report = run(repo, vcpkg_roots, build_dirs, qt_root)
    for violation in report.violations:
        print(violation)
    for note in report.notes:
        print(f"licensecheck: note: {note}", file=sys.stderr)
    if args.sbom:
        write_sbom(repo, report, args.sbom)
    print(f"licensecheck: {len(report.packages)} package(s), {len(report.violations)} violation(s)",
          file=sys.stderr)
    return 1 if report.violations else 0


if __name__ == "__main__":
    sys.exit(main())
