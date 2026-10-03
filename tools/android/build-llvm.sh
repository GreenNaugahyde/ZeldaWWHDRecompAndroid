#!/bin/bash
# LLVM for the APK that recompiles the game on the device (-PwwhdDeviceRecomp): AArch64 code
# generation and the ORC JIT linker, as static libraries for Android arm64, in build/llvm/install.
# Takes about 20 minutes on a 12-core PC; needs git, CMake, Ninja and the NDK.
set -e
cd "$(dirname "$0")/../.."
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.2.12479018}
mkdir -p build/llvm
cd build/llvm
if [ ! -d src ]; then
  git clone --depth 1 --branch llvmorg-20.1.8 https://github.com/llvm/llvm-project.git src
fi
# host tools (tablegen) of the same version
cmake -S src/llvm -B host -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF
ninja -C host llvm-tblgen llvm-config
# Android arm64 libraries (static), no tools
cmake -S src/llvm -B android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DLLVM_HOST_TRIPLE=aarch64-linux-android -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_TABLEGEN=$PWD/host/bin/llvm-tblgen \
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=OFF \
  -DCMAKE_INSTALL_PREFIX=$PWD/install
ninja -C android install
cp src/llvm/LICENSE.TXT install/LICENSE.TXT  # shown in the app (Apache-2.0 with LLVM exception)
du -sh install
