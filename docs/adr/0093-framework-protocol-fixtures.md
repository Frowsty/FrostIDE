# Explicit Catch2 and Boost.Test protocol profiles

Status: implemented in source; runtime acceptance deferred by the user's implementation-first instruction.

RUN-9 uses real CTest-discovered executable commands. Users explicitly select the Catch2 XML schema-3 or Boost.Test 1.85 protocol in the Unit Tests context menu. Binary names and source macros do not establish a framework. Existing CTest, Google Test, doctest and engine providers remain independent.

Catch2 listing requires the reporter-customized list protocol introduced in 3.0.1; completed run documents must identify major version 3 and XML format version 3. The authored real acceptance fixture is exactly Catch2 3.7.1. Older/other XML schemas are rejected, so this does not claim blanket Catch2 2/3 compatibility. Boost.Test listing uses its DOT tree, which distinguishes suites from cases; its run uses detailed XML outcomes and a separate XML log with explicit `BuildInfo boost="1.85.0"`. Different Boost versions are refused by this profile.

These are test-only external dependencies, not downloaded, vendored or bundled into FrostIDE. Both upstream projects use BSL-1.0. The provisioner must retain upstream licence notices with installed packages; no upstream implementation code or prebuilt framework libraries are redistributed here. Authored fixtures and parser specimens remain Apache-2.0. `tests/run/fixtures/frameworks/profiles.json` records exact versions, upstream tags, licence identifiers and package entry points.

Future acceptance provisioning: install Catch2 3.7.1 and Boost 1.85.0 in an external tool prefix, supply it through `CMAKE_PREFIX_PATH`, then enable `FROST_BUILD_FRAMEWORK_ACCEPTANCE`. Both `find_package` calls are EXACT/REQUIRED. Missing dependencies fail configuration; there is no runtime skip and no automatic fetch. The option registers real adapter acceptance and the corresponding app UI case. The standalone fixture CMake project has the same exact requirements. No fixture was built or launched during implementation.

Protocol references:
- [Catch2 3.7.1 XML reporter](https://github.com/catchorg/Catch2/blob/v3.7.1/src/catch2/reporters/catch_reporter_xml.cpp)
- [Catch2 command-line documentation](https://catch2-temp.readthedocs.io/en/latest/command-line.html)
- [Boost.Test 1.85 DOT reporter](https://github.com/boostorg/test/blob/boost-1.85.0/include/boost/test/impl/unit_test_main.ipp)
- [Boost.Test 1.85 XML results](https://github.com/boostorg/test/blob/boost-1.85.0/include/boost/test/impl/xml_report_formatter.ipp)
- [Boost.Test 1.85 XML log](https://github.com/boostorg/test/blob/boost-1.85.0/include/boost/test/impl/xml_log_formatter.ipp)
- [Boost.Test exact/repeatable filters](https://www.boost.org/doc/libs/1_85_0/libs/test/doc/html/boost_test/utf_reference/rt_param_reference/run_test.html)
