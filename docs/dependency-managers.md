# Dependency managers: implemented local workflows

The **Install or Restore Dependencies...** action opens a concrete operation form.
It performs filesystem discovery only until the user reviews the executable and
literal argument vector and explicitly accepts execution. Input folders require
current project trust. A consent receipt retains primary manifest/profile bytes
and trust revision; the worker replans immediately before launch and refuses stale
inputs, escaped output links, revoked trust or cancellation. This is execution of
trusted package recipes, not a sandbox for those recipes.

Supported operations:

- vcpkg manifest install through an absolute configured executable, or the existing
  `$FROST_SDK/vcpkg` executable. Optional local overlay ports and manifest filesystem
  registries work. `--no-downloads`, cleared binary caches and disabled metrics are
  used; no tool is downloaded or bootstrapped. CMake setup uses the installed tree,
  explicit triplet and `VCPKG_MANIFEST_INSTALL=OFF`, so configure does not silently
  run another package-manager install.
- Conan 2 install with two explicit local host/build profile files, CMakeToolchain
  and CMakeDeps, `--no-remote`, and missing-package builds from the existing local
  Conan cache. The supplied no-layout recipe profile expects
  `<output>/conan_toolchain.cmake`. Custom recipe generator layouts are reported
  when that output is absent; they require manual toolchain selection.
- Windows NuGet.exe restore of existing native `packages.config` from an explicit
  local feed into the selected workspace-relative packages folder. Existing native
  project imports consume those restored package targets. PackageReference, CPM,
  package search/install/update/remove and remote-feed editing are not implemented.

Successful CMake setup offers a named personal preset using the generated
`toolchainFile` explicitly, preserving the existing presets' bytes through the
shared kit writer. The normal watcher/profile selector reloads it, and ordinary
configure retains its trust gate. No manager is invoked merely by selecting a
profile. Existing preset-name conflicts refuse a write.

For a loaded native project, vcpkg setup can request early configuration-scoped
MSBuild properties and import hooks through the ordinary ProjectHost transaction
path. The helper's `earlyProperties` insertion preserves conventional config/import
order; missing anchors refuse. Directly declared custom ForceImport hooks are
refused rather than replaced. The application rechecks context before submission.
This integration depends on the MSBuild SetProperties implementation, not on raw
XML rewriting in the dependency UI.

Fixtures and later-execution tests are authored under
`tests/build/fixtures/dependencies`: a filesystem vcpkg registry with a local
header-only port, a Conan recipe/consumer with Windows/Linux profiles, and a tiny
native NuGet archive and consuming vcxproj. The real suite requires absolute
`FROST_TEST_VCPKG`, `FROST_TEST_CONAN`, `FROST_TEST_CMAKE`, `FROST_TEST_NINJA`, and
(on Windows) `FROST_TEST_NUGET` / `FROST_TEST_MSBUILD`. Conan additionally requires a
separate `FROST_TEST_CONAN_HOME` preseeded with the supplied local recipe; no test
silently downloads it. Optional `FROST_TEST_CONAN_HOST_PROFILE` and
`FROST_TEST_CONAN_BUILD_PROFILE` select the actual installed compiler profiles.
Missing prerequisites fail rather than skip. Real tests restore/install, configure
and build actual consumers; these tests have not been executed.

No builds, tests, manager invocations, installs, WSL or verification runners were
executed in this implementation-first round. Source clearance is separate from
runtime acceptance, and these scoped workflows do not establish all R10 exits.

Protocol references:
- https://learn.microsoft.com/en-us/vcpkg/users/manifests
- https://learn.microsoft.com/en-us/vcpkg/maintainers/registries
- https://docs.conan.io/2/reference/commands/install.html
- https://learn.microsoft.com/en-us/nuget/reference/cli-reference/cli-ref-restore
