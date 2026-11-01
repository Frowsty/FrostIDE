# ADR 0033: pugixml 1.16 for the static project readers

- Status: accepted
- Date: 2026-11-01
- Amended: 2026-11-01 by ADR 0029 (M2 evaluates through path (c); ProjectHost is M16's; nothing else
  changes)
- Plan reference: PLAN.md section 2 ".sln / .slnx" (".slnx via pugixml") and "Dependencies" (pugixml from vcpkg);
  ADR 0032 (W1); CLAUDE.md "Licences"

## Context
W1 (ADR 0032) needs `src/project` to read `.slnx`, `.vcxproj` and `.vcxproj.filters`, which are XML, without Qt
(only `src/ui` and `src/app` may use Qt, so QXmlStreamReader is out) and without MSBuild. No dependency already in
the tree parses XML. PLAN.md already lists pugixml among the vcpkg dependencies and names it for `.slnx`.

## Decision
1. **pugixml 1.16 from the vcpkg baseline** (9eec002; port `pugixml`, licence `MIT`, on the `tools/licensecheck.py`
   allowlist), default features (no `compact`), static like every other port (our triplets). `vcpkg.json` lists it
   with `version>=` 1.16; the root CMakeLists finds it next to SQLite and xxHash (`if(NOT FROST_FUZZ)`: fuzz trees
   install no ports and build neither storage nor project).
2. **Private to frost::project.** `frost_project` links `pugixml::pugixml` PRIVATE and only its own sources include
   `src/project/src/xml.hpp`, so no layer above project sees `pugixml.hpp`.
3. **Parsing rules.** `load_buffer` with `parse_default` and `encoding_auto` (BOMs, UTF-16 and CRLF handled by
   pugixml). DOCTYPEs are skipped and pugixml never loads external entities, so a project file cannot make the reader
   fetch anything. XPath is not used. Element and attribute names are compared ASCII-case-insensitively, as MSBuild
   does. A parse error is `Corrupt` with the line of the problem.
4. **Static reading only.** Imported `.props`/`.targets` are not evaluated; that is path (c)'s MSBuild evaluation
   (M2; ProjectHost replaces it in M16, ADR 0029). The header comments of `solution_file.hpp` and `vcxproj.hpp` say what is and is not read.

## Consequences
- The readers stay small and Qt-free; path (c) (M2) and ProjectHost (M16) can replace or cross-check them later
  without touching the UI.
- pugixml's notice reaches binary distributions through the SBOM and notices (M5), like xxHash's.
- Deferred to hardening: a fuzz target for the readers (needs a fuzz tree with ports, as ADR 0014 notes for the
  journal) and a size cap on project files before parsing.

## Alternatives considered
- **QXmlStreamReader:** Qt is banned below `src/ui`.
- **A hand-written XML subset parser:** error-prone (entities, CDATA, encodings) for no gain.
- **expat or libxml2:** C APIs, larger; libxml2 brings optional network and DTD features we would have to disable.
- **tinyxml2 (Zlib):** comparable, but PLAN.md already names pugixml.
