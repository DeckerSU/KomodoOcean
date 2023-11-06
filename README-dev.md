# Scribbles of madman

This branch is planned to be universal for all 3 OS (Linux, Windows, MacOS).
## todo

- Build subsystem: after build win version, if we want to build linux version for example with `build-linux.sh`, univalue and cryptoconditions dirs needs to be cleaned with `make clean` from previous build. otherwise it will not be linked
- Check Mac build with static libs
- Remove unneeded `build-*.sh` files
- Check *.deb package build

## dev notes

### part 1

This file contains only **dev** notes. It's a some kind of notepad or "scribbles of madman" ...

- https://github.com/bitcoin/bitcoin/pull/3914/files - Build fully static versions of bitcoind and bitcoin-cli for older Linux distros
- ```./configure --enable-glibc-back-compat --prefix=$(pwd)/depends/x86_64-pc-linux-gnu LDFLAGS="-static-libstdc++"```
- https://bitcointalk.org/index.php?topic=1080289.0;all -  [HOWTO] compile altcoin for windows on linux using mxe and mingw


```
objdump -T src/qt/komodo-qt | grep 2.25
0000000000000000      DF *UND*	0000000000000000  GLIBC_2.25  __explicit_bzero_chk
0000000000000000  w   DF *UND*	0000000000000000  GLIBC_2.25  getentropy
```

- https://stackoverflow.com/questions/44488972/static-libstdc-works-on-g-but-not-on-pure-gcc
- https://thecharlatan.github.io/GLIBC-Back-Compat/ - Make binaries portable across linux distros

```
-Wl,--wrap=log2f

AX_CHECK_LINK_FLAG([[-Wl,--wrap=__divmoddi4]], [COMPAT_LDFLAGS="$COMPAT_LDFLAGS -Wl,--wrap=__divmoddi4"])
AX_CHECK_LINK_FLAG([[-Wl,--wrap=log2f]], [COMPAT_LDFLAGS="$COMPAT_LDFLAGS -Wl,--wrap=log2f"])
```

- https://drewdevault.com/2016/07/19/Using-Wl-wrap-for-mocking-in-C.html

### part 2

MacOS static build now produce the following binary:

```
src/qt/komodo-qt:
	/usr/local/opt/gcc@6/lib/gcc/6/libstdc++.6.dylib (compatibility version 7.0.0, current version 7.22.0)
	/System/Library/Frameworks/DiskArbitration.framework/Versions/A/DiskArbitration (compatibility version 1.0.0, current version 1.0.0)
	/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit (compatibility version 1.0.0, current version 275.0.0)
	/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation (compatibility version 300.0.0, current version 1570.15.0)
	/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices (compatibility version 1.0.0, current version 944.3.0)
	/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 45.0.0, current version 1671.40.118)
	/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices (compatibility version 1.0.0, current version 50.1.0)
	/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 150.0.0, current version 1570.15.0)
	/System/Library/Frameworks/Security.framework/Versions/A/Security (compatibility version 1.0.0, current version 58286.251.4)
	/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration (compatibility version 1.0.0, current version 963.250.1)
	/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics (compatibility version 64.0.0, current version 1251.12.0)
	/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL (compatibility version 1.0.0, current version 1.0.0)
	/System/Library/Frameworks/AGL.framework/Versions/A/AGL (compatibility version 1.0.0, current version 1.0.0)
	/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon (compatibility version 2.0.0, current version 158.0.0)
	/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText (compatibility version 1.0.0, current version 1.0.0)
	/System/Library/Frameworks/LDAP.framework/Versions/A/LDAP (compatibility version 1.0.0, current version 2.4.0)
	/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 400.9.4)
	/usr/local/opt/gcc@6/lib/gcc/6/libgomp.1.dylib (compatibility version 2.0.0, current version 2.0.0)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1252.250.1)
	/usr/local/lib/gcc/6/libgcc_s.1.dylib (compatibility version 1.0.0, current version 1.0.0)
	/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork (compatibility version 1.0.0, current version 978.0.7)
	/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO (compatibility version 1.0.0, current version 1.0.0)
	/usr/lib/libobjc.A.dylib (compatibility version 1.0.0, current version 228.0.0)
```

To avoid dynamic linking from `/usr/local/lib/gcc/6/` need to add `-static-libgcc` linker flag, but binary compiled and linked this way with gcc6 produced some exceptions right after launch, probably due this:

https://stackoverflow.com/questions/50920999/why-dont-exceptions-work-with-gcc7-and-static-libgcc-on-osx

So, the solution is build with gcc6, but link with gcc8, like `../libtool  --tag CXX  --mode=link g++-8 -g -O2 <...> -static-libgcc` .

### part 3 (optimisations)

since we have [Add SSE4 optimized SHA256](https://github.com/DeckerSU/KomodoOcean/commit/9c22593e70b7ee493767e8a469173c2c85b09620) and `--enable-experimental-asm` build flag, probably we need to change SHA256 implementation in other places, like:

`komodo_utils.h`
```
void vcalc_sha256(char deprecated[(256 >> 3) * 2 + 1],uint8_t hash[256 >> 3],uint8_t *src,int32_t len)
{
    /*
    struct sha256_vstate md;
    sha256_vinit(&md);
    sha256_vprocess(&md,src,len);
    sha256_vdone(&md,hash);
    */

    /*
    uint256 sha256;
    CSHA256().Write((const unsigned char *)src, len).Finalize(sha256.begin());
    for (int i = 0; i < 32; i++) hash[i] = ((uint8_t *)&sha256)[i];
    */

    CSHA256().Write((const unsigned char *)src, len).Finalize(hash);
}
```
bcz few procedures uses `vcalc_sha256` even to check incoming block, so it needs to use fast sha256 implementation in case if we have experimental asm enabled.

[x] Already done.

### part 4

- Probably we have some unexpected / unpredictable behavior (strange side-effect) with this [linux_release_CFLAGS: -O1 -> -O2](https://github.com/DeckerSU/KomodoOcean/commit/511b27aba1c53b05acdc2a169eb374017b7b9145) commit. When wallet built with `CXXFLAGS='-g0 -O2'` all is ok, when with `CXXFLAGS='-g -O2'` - it's ok too. But when we tried to build wallet with just `CXXFLAGS='-g'` in `zcutil/build.sh ` it crashed somewhere in init of [CDBEnv](https://github.com/DeckerSU/KomodoOcean/blob/b8d315bbbece1cb3786855dae40de70a3f8385f0/src/wallet/db.cpp#L49). So, i just want to mention it here. In future it should be investigated to find a solution, may be commit should be reverted or additional BDB build flags should be added.

### part 5

- If you want to build under Linux for multiple OSes from the same repo / folder, like, build for Win64 and then build for MacOS - you can do the following:
```
make clean
make -C src/univalue clean
make -C src/cryptoconditions clean
rm src/qt/moc_*.cpp
```
after each different build.

### part 6

For some reason moving out `PKG_CONFIG_SYSROOT_DIR`, `PKG_CONFIG_LIBDIR`, `QT_MAC_SDK_NO_VERSION_CHECK` environment variables into $(package)_config_env as mentione here - https://github.com/bitcoin/bitcoin/commit/affbf58a1e52a8e60c830be6a9e0347e0ff4c28e doesn't work for Qt package build, so we define them in `$(package)_config_cmds` in `qt.mk`.

### part 7

Valgrind usage example:

- https://stackoverflow.com/questions/5134891/how-do-i-use-valgrind-to-find-memory-leaks

```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         --log-file=valgrind-out.txt \
         ./src/komodo-test --gtest_filter=ParseArgumentsTests.*:DeckerTests.*
```
