# 影片解码器（pl_mpeg → WebAssembly）

`movie.c` 是 **pl_mpeg** 的 WebAssembly 包装；`pl_mpeg.h` / `movie.c` 一起编译成 `movie.wasm`。
pl_mpeg 是 Dominic Szablewski 的 MIT 单文件库（MPEG-1 video + MP2 audio + MPEG-PS demux）。
本项目只启用视频，音频在 `movie_open` 里显式关闭。

- 上游：https://github.com/phoboslab/pl_mpeg
- 许可证：MIT（见 `pl_mpeg.h` 文件头）
- 生成：`movie.wasm` 是派生构建产物（49 KB）；`movie.c` 只导出 `movie_*`，不暴露 pl_mpeg 内部。
- 构建（一次性，需要 WASI SDK 24；不在日常 `pnpm` 路径里）：

```sh
wasi-sdk/bin/clang --target=wasm32-unknown-unknown -O3 -nostdlib -fno-builtin \
  -I. -I<stub-include> \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
  -o movie.wasm movie.c
```

`movie.c` 自己提供 `malloc/free/realloc/memcpy/memset/memmove/abs`（`-nostdlib`），
用 `__heap_base` 向上的 bump allocator；`movie_open` 的输入字节必须由调用方复制进 WASM 内存。
解码输出是 top-down RGBA8，alpha 由包装强制为 255；不读音频。
