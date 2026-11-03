# SPDX-License-Identifier: MIT
# FrostIDE clang-cl-only overlay: the MSBuild-only host dependency is unused; see ADR 0075.
# libsodium has a special branching/tagging scheme, where regular version tags can actually be moved
# as new patches are applied to that version. This means that we may get unexpected hash mismatches
# when the upstream tag points to a new commit. To avoid this, we must make sure that we always
# use a '-RELEASE' tag, since those seem to be fixed to a single commit.
# See https://github.com/jedisct1/libsodium/issues/1373#issuecomment-2135172301 for more info.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jedisct1/libsodium
    REF "${VERSION}-RELEASE"
    SHA512 f8e11ad193037b7b885a40b832da331105f4e5943b74c0297fe07e02a313786016c777aebb094ad1fcf16398e54af545c2d55404c28b371eae5922d1e164ba00
    HEAD_REF master
    PATCHES
        001-mingw-i386.patch
)

# The msbuild solution only builds with MSVC; other compilers targeting
# windows (e.g. clang, in cross builds or clang-based triplets) go through
# the make-based path like every other platform.
set(USE_MSBUILD OFF)
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    vcpkg_cmake_get_vars(cmake_vars_file)
    include("${cmake_vars_file}")
    if(VCPKG_DETECTED_CMAKE_C_COMPILER MATCHES "[/\\\\]cl\\.exe$")
        message(FATAL_ERROR "FrostIDE libsodium overlay requires the clang-cl triplet")
    endif()
endif()

if(USE_MSBUILD)
    set(lib_linkage "LIB")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        set(lib_linkage "DLL")
    endif()

    set(LIBSODIUM_PROJECT_SUBPATH "builds/msvc/vs2022/libsodium/libsodium.vcxproj" CACHE STRING "Triplet variable")

    vcpkg_msbuild_install(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_SUBPATH "${LIBSODIUM_PROJECT_SUBPATH}"
        RELEASE_CONFIGURATION "Release${lib_linkage}"
        DEBUG_CONFIGURATION "Debug${lib_linkage}"
    )

    file(INSTALL "${SOURCE_PATH}/src/libsodium/include/sodium.h" "${SOURCE_PATH}/src/libsodium/include/sodium" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/src/libsodium/include/sodium/version.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include/sodium")
    file(REMOVE "${CURRENT_PACKAGES_DIR}/include/Makefile.am" "${CURRENT_PACKAGES_DIR}/include/sodium/version.h.in")

    block(SCOPE_FOR VARIABLES)
        set(PACKAGE_NAME "libsodium")
        set(PACKAGE_VERSION "${VERSION}")
        set(prefix [[unused]])
        set(exec_prefix [[${prefix}]])
        set(includedir [[${prefix}/include]])
        set(libdir [[${prefix}/lib]])
        set(PKGCONFIG_LIBS_PRIVATE "")
        configure_file("${SOURCE_PATH}/libsodium.pc.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libsodium.pc" @ONLY)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libsodium.pc" " -lsodium" " -llibsodium")
        if(NOT VCPKG_BUILD_TYPE)
            set(includedir [[${prefix}/../include]])
            configure_file("${SOURCE_PATH}/libsodium.pc.in" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libsodium.pc" @ONLY)
            vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libsodium.pc" " -lsodium" " -llibsodium")
        endif()
    endblock()
else()
    if(VCPKG_TARGET_IS_EMSCRIPTEN)
        list(APPEND OPTIONS "--disable-ssp" "--disable-asm")
    endif()
    if(NOT VCPKG_TARGET_IS_MINGW)
        list(APPEND OPTIONS --disable-pie)
    endif()
    if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
        if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
            message(FATAL_ERROR "Shared libsodium is not implemented for non-MSVC compilers on Windows: libtool silently builds an unusable static archive for this target. Build libsodium static, or with MSVC.")
        endif()
        # getpid links via the oldnames.lib compatibility alias, so
        # configure's link check false-passes; no header declares it for
        # the MSVC CRT, breaking compilation. The msbuild-built binaries
        # never define HAVE_GETPID either.
        list(APPEND OPTIONS "ac_cv_func_getpid=no")
        # libtool extracts convenience archives using `ar x`. llvm-lib does not
        # implement the MSVC /EXTRACT command used by vcpkg's ar-lib wrapper.
        cmake_path(REPLACE_FILENAME VCPKG_DETECTED_CMAKE_AR llvm-ar.exe OUTPUT_VARIABLE frost_ar)
        if(NOT EXISTS "${frost_ar}")
            message(FATAL_ERROR "Pinned llvm-ar is required for libsodium archives")
        endif()
        # Use GNU-style archive flags too; the triplet's -machine/-nologo flags
        # belong to llvm-lib and would make llvm-ar print help without an archive.
        list(APPEND OPTIONS "AR=${frost_ar}" "ARFLAGS=cr")
        # The debug CRT's recursive abort probe waits for a runtime dialog.
        # Conservatively report no catchable SIGABRT; this macro only controls
        # libsodium's misuse test, not library implementation or crypto features.
        list(APPEND OPTIONS_DEBUG "ax_cv_check_cCATCHABLE_ABRT=no")
        # The library's own objects must not reference its symbols through
        # dllimport; export.h defaults to that when neither SODIUM_STATIC nor
        # SODIUM_DLL_EXPORT is defined, which the make build does not do.
        vcpkg_replace_string("${SOURCE_PATH}/src/libsodium/include/sodium/export.h" "#ifdef SODIUM_STATIC" "#if 1")
    endif()

    set(frost_configure_commands)
    if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
        # Autoconf expands CFLAGS/LDFLAGS as shell words. The pinned SDK paths
        # contain spaces, so carry those exact compiler pins in a response file.
        # Do not pass lld-link's /vctoolsdir syntax to clang-cl via libtool escapes.
        function(frost_sodium_configure_flags config)
            set(link_flags "${VCPKG_COMBINED_SHARED_LINKER_FLAGS_${config}}")
            foreach(pin IN ITEMS vctoolsdir winsdkdir)
                string(REGEX MATCH "[/-]${pin}:\"([^\"]+)\"" match "${link_flags}")
                if(NOT match)
                    message(FATAL_ERROR "Missing pinned ${pin} for libsodium")
                endif()
                set(${pin} "${CMAKE_MATCH_1}")
                string(REPLACE "${match}" "" link_flags "${link_flags}")
            endforeach()
            string(REGEX MATCH "[/-]winsdkversion:([0-9.]+)" match "${link_flags}")
            if(NOT match)
                message(FATAL_ERROR "Missing pinned SDK version for libsodium")
            endif()
            set(sdk_version "${CMAKE_MATCH_1}")
            string(REPLACE "${match}" "" link_flags "${link_flags}")
            set(pin_file "${CURRENT_BUILDTREES_DIR}/frost-pins-${config}.rsp")
            # Hide MSVC-only flags such as -RTC1 from libtool's unrelated -R parser.
            file(WRITE "${pin_file}" "${VCPKG_COMBINED_C_FLAGS_${config}}\n\"-vctoolsdir${vctoolsdir}\"\n\"-winsdkdir${winsdkdir}\"\n-winsdkversion${sdk_version}\n")
            set(CFLAGS_${config} "@${pin_file}" PARENT_SCOPE)
            set(CXXFLAGS_${config} "@${pin_file}" PARENT_SCOPE)
            set(LDFLAGS_${config} "${link_flags}" PARENT_SCOPE)
        endfunction()
        list(APPEND frost_configure_commands frost_sodium_configure_flags)
    endif()
    vcpkg_make_configure(
        AUTORECONF
        SOURCE_PATH "${SOURCE_PATH}"
        PRE_CONFIGURE_CMAKE_COMMANDS ${frost_configure_commands}
        OPTIONS ${OPTIONS}
        OPTIONS_DEBUG ${OPTIONS_DEBUG}
    )
    if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
        # vcpkg_make_install recomputes ARFLAGS from the MSVC triplet. A make
        # command-line assignment also reaches recursive make/libtool recipes.
        vcpkg_make_install(OPTIONS "ARFLAGS=cr")
    else()
        vcpkg_make_install()
    endif()

    file(REMOVE_RECURSE
        "${CURRENT_PACKAGES_DIR}/debug/include"
        "${CURRENT_PACKAGES_DIR}/debug/share"
    )
endif()

vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/sodium/export.h" "#ifdef SODIUM_STATIC" "#if 1" IGNORE_UNCHANGED)
endif()

# vcpkg legacy
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/sodiumConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/unofficial-sodium/unofficial-sodiumConfig.cmake"
    @ONLY
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
