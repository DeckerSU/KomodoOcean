#!/usr/bin/env bash

# If `make clean` fails for any reason, it is helpful to
# manually clean any remaining files from the previous build.
# This is especially useful if the new build is for a different
# architecture than the previous build, such as a Windows release
# after an OSX cross-compile. Normally these files should be
# cleaned during `make clean` execution.

rm ./src/qt/moc_*.cpp
rm ./src/qt/*.moc

# this will partially recreate *.Po and *.Plo in .deps

# https://unix.stackexchange.com/questions/84265/linux-shell-find-exec-in-find-exec
find ./src/ -name ".deps" -type d -exec sh -c 'find {} -name "*.Po" -type f -delete' \;
find ./src/ -name ".deps" -type d -exec sh -c 'find {} -name "*.Plo" -type f -delete' \;

# https://askubuntu.com/questions/377438/how-can-i-recursively-delete-all-files-of-a-specific-extension-in-the-current-di
find ./src/ -name "*.Po" -type f -delete
find ./src/ -name "*.Plo" -type f -delete
find ./src/ -name ".dirstamp" -type f -delete

rm ./src/qt/libkomodoqt.a
rm ./src/leveldb/libmemenv.a
rm ./src/leveldb/libleveldb_sse42.a
rm ./src/leveldb/libleveldb.a
