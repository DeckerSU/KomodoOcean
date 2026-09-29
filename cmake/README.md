# Independent Linux CMake build

This build lives entirely under `cmake/`. The existing Autotools build is not
invoked for the application and no existing source or build recipe is modified.
`depends` is the **permanent dependency builder**, run from a private copy of its
recipes. It still uses the upstream build tools required by each package.

Currently supported: native Linux x86_64, CMake 3.22 or newer, GCC or Clang,
Ninja or Unix Makefiles. Other platforms/cross compilation fail explicitly;
the dependency adapter and target/platform checks are separate extension points.

## Build

Install the prerequisites needed by the existing `zcutil/build.sh`/`depends`
build (C/C++ toolchain, GNU Make, autoconf, automake, libtool, pkg-config, patch,
curl, archive tools, Python, Perl, and the platform development headers), plus
CMake and optionally Ninja. Dependency recipes pin and install their own Rust
toolchain. CMake does not install system packages.

From the repository root:

```sh
cmake -S cmake -B /tmp/komodo-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKMD_JOBS=4
cmake --build /tmp/komodo-build
```

The first build compiles dependencies from source, then configures and builds
the application. `KMD_JOBS` controls the nested builds; an outer `--parallel`
does not replace it. Programs are in `/tmp/komodo-build/bin/`:
`komodod`, `komodo-cli`, `komodo-tx`, `wallet-utility`, `fetch-params`, and
`komodo-qt`. The default configuration matches the principal features of
`zcutil/build.sh`: wallet, Rust, mining, ZMQ, Qt5, hardening and consensus library;
tests and BIP70 are off. UPnP is off (the current depends package set does not
provide miniupnpc). Wallet/Rust remain required in this implementation.

```sh
# Headless build, with a separate dependency variant that omits Qt.
cmake -S cmake -B /tmp/komodo-headless -G Ninja -DKMD_BUILD_GUI=OFF
cmake --build /tmp/komodo-headless

# Enable the existing GoogleTest suite.
cmake -S cmake -B /tmp/komodo-build -DKMD_BUILD_TESTS=ON
cmake --build /tmp/komodo-build
ctest --test-dir /tmp/komodo-build --output-on-failure

# Install programs, consensus archive/header and Qt's shared dependencies.
cmake --install /tmp/komodo-build --prefix /tmp/komodo-install
```

Options: `KMD_BUILD_GUI`, `KMD_BUILD_TESTS`, `KMD_BUILD_CONSENSUS`,
`KMD_ENABLE_MINING`, `KMD_ENABLE_ZMQ`, `KMD_ENABLE_ASM`, `KMD_HARDENING`,
`KMD_JOBS`. Standard single-configuration build types are supported. Assertions
stay enabled in optimized builds, as in the original build. Debug application
builds use release dependencies, avoiding Boost's incompatible debug STL ABI.

## Isolation and caches

```text
<build>/
  depends/
    sources/                        downloaded source archives and checksums
    <triplet>-<toolchain-id>/
      recipes/depends/              copied tracked recipes, patches, scripts
      work/                         extraction, compilation, staging
      cache/                        compiled dependency packages
      prefix/<triplet>/             installed dependencies and native tools
  app/
    source/src/                     clean, automatically updated source snapshot
    native/                         host code generators
    lib/                            application archives
    compile_commands.json
  bin/
```

No existing objects, libraries, generated headers, or package caches from the
repository's `depends/` or `src/` are consumed. Supplying the legacy depends
prefix explicitly is rejected. Compiler identity/version, flags and the GUI
variant separate the binary cache directories; depends additionally hashes its
recipes and patches. Use a fresh build directory when changing the system SDK
or other external toolchain inputs. Builds are not claimed to be bit-reproducible
across arbitrary Linux distributions.

`KMD_DOWNLOAD_CACHE` can select a source archive cache. By default it is private.
To avoid downloading archives already available locally, **copy source archives
only** into `<build>/depends/sources/` before building. Never copy the old `built/`,
`work/`, or `<triplet>/` trees. Downloads are checked by the unchanged recipes.
Fully offline builds require all recipe archives to be present beforehand.

Qt itself is static; some XCB dependencies are shared, as prescribed by the
existing recipes. They remain inside the private prefix. Installation copies
these shared libraries into `lib/komodoocean` and uses a relative ELF RPATH.
System libc, libstdc++, libgcc and recipe-selected zlib remain system dependencies.
This is not a fully static executable build.

## Development and existing private prefixes

Build only the dependencies with `cmake --build <build> --target depends`.
Completed dependencies are reused until their inputs change. To recheck package
checksums and recreate a damaged private prefix from its cache, use
`cmake --build <build> --target depends-refresh` (automatic dependency mode only).
Afterwards the inner project can be used directly by an IDE:

```sh
cmake -S cmake/app -B /tmp/komodo-dev -G Ninja \
  -DKMD_DEPENDS_PREFIX=/absolute/path/to/private/prefix/x86_64-pc-linux-gnu
cmake --build /tmp/komodo-dev --parallel 4
```

The outer project accepts the same `KMD_DEPENDS_PREFIX` option to skip dependency
preparation. It must be a complete, compatible prefix from a separate build;
it is used read-only. Do not relocate a prefix: Qt's pkg-config metadata contains
absolute paths. CMake reports missing libraries rather than falling back to
system Boost, Qt, OpenSSL or Berkeley DB. BDB 6.2 is required for wallet compatibility.

Sources are copied because quoted includes in this legacy code can otherwise
find old Autotools-generated headers beside the original source before any
CMake include directory. `configure_file(COPYONLY)` tracks changes and preserves
timestamps when contents do not change. Editing existing original sources
automatically refreshes the snapshot on the next build. Do not edit the copy.
The checked-in manifests also make source archives without `.git` usable.
After adding/removing tracked sources, run `python3 cmake/scripts/update-inputs.py`
and review `targets/Sources.cmake`. Untracked experiments are intentionally excluded.

The build does not fetch runtime proving parameters or start a node. GUI help
can be checked without a display using `QT_QPA_PLATFORM=minimal`. Packaging
(DEB/RPM/AppImage), release signing and RPC integration-test orchestration are
outside this build's current scope.

The source-isolation regression checks require only Python and CMake:
`python3 cmake/scripts/test-isolation.py`.

## Validation

Validated with GCC 11.4.0 and CMake 3.22.1 on Linux x86_64:

- Fresh isolated dependency compilation, seeded only with source archives.
- Full Release build including Qt5, command-line tools and consensus archive.
- 108 existing GoogleTest tests passed through CTest (9 upstream-disabled tests).
- Two source-isolation regression tests passed.
- Crypto and secp256k1 targets compiled with ASM, mining, ZMQ and GUI disabled.
- CLI version/help checks and Qt help with the minimal platform plugin.
- Installation to a separate prefix; installed GUI starts and resolves its
  bundled libraries without referring to the build prefix.
- An unchanged repeat build does not compile objects or rebuild dependencies.
- Outer configuration with both Ninja and Unix Makefiles.

Interactive GUI operation, cross compilation and other compilers/platforms
have not been validated here. Runtime proving parameters are not needed for
these checks and are not downloaded by the build.
