# cmake/DeployWindows.cmake
#
# Collects all non-system runtime DLL dependencies of BINARY into DEPLOY_DIR
# using file(GET_RUNTIME_DEPENDENCIES).  Called from an install(CODE "...")
# block in src/server/CMakeLists.txt so that CPack picks up everything it
# copies.
#
# Qt DLLs are intentionally NOT excluded here so that Qt's own transitive
# dependencies (the three ICU DLLs: icuuc, icudt, icuin) are resolved and
# bundled.  windeployqt runs afterwards and overwrites them with "up to date"
# — no conflict, and the ICU DLLs are guaranteed to be present.
#
# Required variables (set by the caller before include()-ing this script):
#   BINARY      — absolute path to the installed server .exe
#   DEPLOY_DIR  — destination directory (typically CMAKE_INSTALL_PREFIX/bin)
#   VCPKG_BIN   — vcpkg installed/x64-windows/bin, searched for DLLs
#   QT_BIN      — Qt6 bin directory, searched for Qt and ICU DLLs

cmake_minimum_required(VERSION 3.18)

# ── Pre-exclude: patterns matched against the bare DLL filename ────────────
# Windows API forwarder stubs — always virtual, never a real file on disk.
# MSVC CRT / VCRedist DLLs — handled separately by InstallRequiredSystemLibraries.
# Qt DLLs are NOT excluded — their transitive ICU dependencies must be walked.
set(_pre_exclude_regexes
    "api-ms-win-.*\\.dll"
    "ext-ms-.*\\.dll"
    "vcruntime.*\\.dll"
    "msvcp.*\\.dll"
    "vccorlib.*\\.dll"
    "concrt.*\\.dll"
)

# ── Post-exclude: patterns matched against the resolved full path ──────────
# Exclude anything that resolves into the Windows system directories.
set(_post_exclude_regexes
    ".*[Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
    ".*[Ww][Ii][Nn][Ss][Xx][Ss][/\\\\].*"
)

# ── Resolve dependencies ───────────────────────────────────────────────────
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES             "${BINARY}"
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    DIRECTORIES             "${VCPKG_BIN}" "${QT_BIN}"
    PRE_EXCLUDE_REGEXES     ${_pre_exclude_regexes}
    POST_EXCLUDE_REGEXES    ${_post_exclude_regexes}
)

# ── Copy resolved DLLs into the deploy directory ───────────────────────────
foreach(_dep IN LISTS _resolved)
    get_filename_component(_name "${_dep}" NAME)
    message(STATUS "  bundle: ${_name}  (${_dep})")
    file(INSTALL "${_dep}" DESTINATION "${DEPLOY_DIR}")
endforeach()

# ── Warn about anything that could not be located ─────────────────────────
# Unresolved DLLs usually mean the vcpkg bin dir is wrong or a release build
# linked against a DLL that is not in VCPKG_BIN.  They are printed as
# warnings rather than errors so the build does not fail — but the resulting
# ZIP may be incomplete.
if(_unresolved)
    foreach(_dep IN LISTS _unresolved)
        message(WARNING "  unresolved (not bundled): ${_dep}")
    endforeach()
endif()

message(STATUS "Windows DLL deploy complete → ${DEPLOY_DIR}")
