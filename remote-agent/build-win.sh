#!/usr/bin/env bash
# 在 Windows 上用 NDK 交叉编译 remote-agent（cmake 方式）。
#
# 与旧版直接调 clang++ 的区别：
#   - libcurl + mbedtls 从源码编译并静态链接，原生支持 HTTPS
#   - 不依赖系统库（OpenSSL / Schannel）
#   - 使用 cmake 构建，依赖关系由 CMakeLists.txt 管理
#
# 用法（Git Bash）：
#   ./build-win.sh            # 产物 build-cmake/remote-agent-v8.bin
set -euo pipefail

NDK=${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}
[ -n "$NDK" ] || {
    echo "请设置 NDK、ANDROID_NDK_HOME 或 ANDROID_NDK_ROOT" >&2
    exit 1
}
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
API=${API:-29}
ABI=${ABI:-arm64-v8a}
OUT_DIR=build-cmake
NAME=${1:-remote-agent-v8}

# cmake：优先使用 PATH，找不到时尝试 Visual Studio 默认安装位置。
CMAKE=${CMAKE:-}
VS_CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
if [ -z "$CMAKE" ] && command -v cmake >/dev/null 2>&1; then
    CMAKE=$(command -v cmake)
elif [ -z "$CMAKE" ] && [ -x "$VS_CMAKE" ]; then
    CMAKE="$VS_CMAKE"
    NINJA="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
fi
[ -n "$CMAKE" ] || { echo "找不到 cmake" >&2; exit 1; }
NINJA=${NINJA:-}

[ -f "$TOOLCHAIN" ] || { echo "找不到 NDK toolchain: $TOOLCHAIN"; exit 1; }

echo "==> cmake 配置（NDK=$NDK, API=$API, ABI=$ABI）"
echo "    cmake=$CMAKE"
rm -rf "$OUT_DIR"

# 选择生成器：有 ninja 就用 Ninja，否则用 Unix Makefiles
if [ -n "$NINJA" ] && [ -x "$NINJA" ]; then
    "$CMAKE" -B "$OUT_DIR" \
        -G "Ninja" \
        -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DRPM_WITH_JPEG=OFF
else
    "$CMAKE" -B "$OUT_DIR" \
        -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DRPM_WITH_JPEG=OFF
fi

echo "==> 编译"
JOBS=$(nproc 2>/dev/null || echo 4)
"$CMAKE" --build "$OUT_DIR" -j"$JOBS"

echo "==> 剥离符号表 & 重命名"
STRIP="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
BIN="$OUT_DIR/remote-agent"
[ -f "$BIN" ] && {
    "$STRIP" --strip-all "$BIN"
    cp "$BIN" "$OUT_DIR/$NAME.bin"
    echo "==> 产物: $OUT_DIR/$NAME.bin"
    ls -l "$OUT_DIR/$NAME.bin"
    "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe" -d "$OUT_DIR/$NAME.bin" | grep -i needed || true
} || {
    echo "编译失败，请检查上面的错误输出"
    exit 1
}
