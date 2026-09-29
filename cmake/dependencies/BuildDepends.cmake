# Existing recipes are the permanent dependency builder. No generated files or
# binary caches from the repository's depends tree are read or written.
set(KMD_DOWNLOAD_CACHE "${CMAKE_BINARY_DIR}/depends/sources" CACHE PATH "Dependency source archives only")
set(no_qt "")
if(NOT KMD_BUILD_GUI)
  set(no_qt 1)
endif()
# depends' own recipe hash does not include all compiler/environment settings.
string(SHA256 toolchain_id "v2|${KMD_HOST}|${CMAKE_C_COMPILER}|${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_C_FLAGS}|${CMAKE_CXX_FLAGS}|${CMAKE_EXE_LINKER_FLAGS}|qt=${KMD_BUILD_GUI}")
string(SUBSTRING "${toolchain_id}" 0 16 toolchain_id)
set(depends_root "${CMAKE_BINARY_DIR}/depends/${KMD_HOST}-${toolchain_id}")
kmd_snapshot("${CMAKE_CURRENT_LIST_DIR}/../depends-inputs.txt" "${depends_root}/recipes")
set(depends_recipes "${depends_root}/recipes/depends")
set(KMD_DEPENDS_PREFIX "${depends_root}/prefix/${KMD_HOST}")
set(depends_command "${CMAKE_COMMAND}" -E env --unset=CONFIG_SITE --unset=MAKEFLAGS --unset=MFLAGS
    --unset=CPATH --unset=C_INCLUDE_PATH --unset=CPLUS_INCLUDE_PATH --unset=LIBRARY_PATH
    --unset=PKG_CONFIG_PATH --unset=PKG_CONFIG_SYSROOT_DIR --unset=CARGO_TARGET_DIR
    --unset=CARGO_BUILD_TARGET --unset=CARGO_ENCODED_RUSTFLAGS --unset=RUSTC --unset=RUSTC_WRAPPER
    "CARGO_HOME=${depends_root}/cargo-home" "RUSTFLAGS="
    "${KMD_MAKE}" -C "${depends_recipes}" "-j${KMD_JOBS}"
    "BUILD=${KMD_HOST}" "HOST=${KMD_HOST}" "NO_QT=${no_qt}" NO_UPNP=1 NO_WALLET= NO_RUST= DEBUG=
    "host_prefix=${KMD_DEPENDS_PREFIX}" "build_prefix=${KMD_DEPENDS_PREFIX}/native"
    "x86_64_linux_prefix=${KMD_DEPENDS_PREFIX}"
    "base_build_dir=${depends_root}/work/build" "base_staging_dir=${depends_root}/work/staging"
    "base_download_dir=${depends_root}/work/download" "BASE_CACHE=${depends_root}/cache"
    "SOURCES_PATH=${KMD_DOWNLOAD_CACHE}"
    "x86_64_linux_CC=${CMAKE_C_COMPILER}" "x86_64_linux_CXX=${CMAKE_CXX_COMPILER}"
    "x86_64_linux_AR=${CMAKE_AR}" "x86_64_linux_RANLIB=${CMAKE_RANLIB}"
    "x86_64_linux_CFLAGS=-pipe ${CMAKE_C_FLAGS}" "x86_64_linux_CXXFLAGS=-pipe ${CMAKE_CXX_FLAGS}"
    "x86_64_linux_LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../depends-inputs.txt" recipe_inputs)
list(TRANSFORM recipe_inputs PREPEND "${KMD_REPOSITORY}/")
set(depends_stamp "${depends_root}/complete.stamp")
add_custom_command(OUTPUT "${depends_stamp}" "${KMD_DEPENDS_PREFIX}/share/config.site"
  COMMAND ${depends_command}
  COMMAND "${CMAKE_COMMAND}" -E touch "${depends_stamp}"
  DEPENDS ${recipe_inputs} "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_CURRENT_LIST_DIR}/../depends-inputs.txt"
  USES_TERMINAL VERBATIM)
add_custom_target(depends DEPENDS "${depends_stamp}" "${KMD_DEPENDS_PREFIX}/share/config.site")
# Explicit recovery/verification, without unpacking the entire prefix on every
# application build (depends' install target always recreates that directory).
add_custom_target(depends-refresh
  COMMAND ${depends_command}
  COMMAND "${CMAKE_COMMAND}" -E touch "${depends_stamp}"
  USES_TERMINAL VERBATIM)
