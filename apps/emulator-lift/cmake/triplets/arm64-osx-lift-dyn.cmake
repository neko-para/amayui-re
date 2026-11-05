# emulator-lift · Unicorn POC 专用的 arm64-osx triplet：**动态**链接（其它 preset 仍 static）。
#
# ★ 为什么不是 static：Unicorn 2.1.4 的静态库自带一个 `crc32`（Castagnoli CRC-32C，见
#   `qemu/util/crc32c.c`），与 zlib 的 `crc32`（libpng / freetype 用它）**同名、不同算法**。
#   static 下 ld64 直接报 duplicate symbol；而"让 unicorn 的版本赢"会让 PNG 的 CRC 校验
#   **静默算错**（⛔ 比链接失败更坏）。动态链接时两边各自绑定自己的 dylib（two-level namespace），
#   互不干扰 —— 这是唯一一个"不改上游源码、不 vendor、语义不静默错"的解法。
# ★ 只影响 唯一的 `macos-arm64` / `out` 构建（VCPKG_TARGET_TRIPLET=arm64-osx-lift-dyn）；
#   默认 `out` 仍是 arm64-osx + static。
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
