#!/usr/bin/env bash

mkdir -p build-fast-release
mkdir -p dist-fast-release

cmake -S . -B build-fast-release -G "Visual Studio 17 2022" -A x64 -T host=x64 -DWINXTERM_BUILD_DSTSHELL_MODE_MUTATOR=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob3 /Oi /Ot /GL /Gy /Gw /arch:AVX2 /favor:INTEL64 /DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob3 /Oi /Ot /GL /Gy /Gw /arch:AVX2 /favor:INTEL64 /DNDEBUG /EHsc" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO"

cmake --build build-fast-release --config Release --target winxterm dstshell --parallel 16

cp -f build-fast-release/Release/winxterm.exe dist-fast-release/winxterm.exe
cp -f build-fast-release/Release/dstshell.exe dist-fast-release/dstshell.exe
rm -f dist-fast-release/dstshell_mode_mutator.exe
rm -f dist-fast-release/*.pdb dist-fast-release/*.ilk dist-fast-release/*.idb dist-fast-release/*.exp dist-fast-release/*.lib

ls -lh dist-fast-release/winxterm.exe dist-fast-release/dstshell.exe
