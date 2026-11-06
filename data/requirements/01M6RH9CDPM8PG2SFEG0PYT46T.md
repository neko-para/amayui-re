# emulator-lift · TITLE 黑场渐变 ~1 fps：full-screen textured quad 回退 CPU

- id: REQ-01M6RH9CDPM8PG2SFEG0PYT46T
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, d3d9, gpu, raster, performance, title]

## 现象（2026-11-06 用户实测）
`--raster gpu --present window --native-overrides all --real-time` 下，LOGO 渐变约 70–120 ms/帧；进入 TITLE 黑场渐变后 0.9–1.5 s/帧，只剩 2–3 个可见台阶。
## 复现
```bash
LIFT_FRAME_TRACE=1 LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=100 \
  ./out/emulator-lift --game-dir <install> --appdata <tmp> \
  --max-frames 80 --real-time --present window --present-hidden \
  --raster gpu --native-overrides all --profile --gpu-strict
```
`--present window` 自动 real-time；不加 real-time 会先卡在虚拟时钟等待，不能看帧率。
## 原始逐帧观测
TITLE 起始约 48–52 帧，之后 wall 0.88–1.50 s、guest 16–35 ms、shim 0.85–1.30 s；`DrawPrimitive` 0.60–1.35 s/帧，图元为 2 三角形 / 4 顶点，像素 894,575–919,601 / 921,600。
`LIFT_SLOW_DRAW` 判定（连续 37 帧）：
```text
[draw] prim=5 verts=4 tris=2 rast=2 pixels=894575 gpu=0 \
  why=GPU phase 1 不支持 separate alpha blend \
  sep_alpha=1 tex=1 blend=5/6 cw=F submit=700–1348ms
```
即每帧一个全屏纹理四边形，`gpu=0`，由 CPU 软光栅逐像素处理。LOGO 的 D3DXSprite 走单独 `rasterDrawTextured()`，所以 LOGO 快、TITLE 慢。`--native-overrides all` 与只开 6 项同段都 ~1 s/帧；`--raster cpu` 更慢。
## 根因
`gpuEligible()` 拒绝 `separate_alpha_blend`，随后也会拒绝 stage0 纹理；TITLE 的 stage0 实际是 `MODULATE(TEXTURE,TFACTOR)`、sampler WRAP+LINEAR、纹理 2048² A8R8G8B8、cw=F、无 alpha test。CPU `rasterTriangle()` 的逐像素 double 路径无法作为最终运行时。
## 2026-11-06 首轮落地（GPU first-class）
- `--raster auto` 缺省：headless⇒CPU（参考后端），window⇒GPU；window 下 GPU 失败 exit 2，不再静默退回 CPU。
- `--gpu-strict`：GPU 启用时 `fallback Draw + sprite fallback > 0` ⇒ 先打印 summary，再 exit 3；与 `--raster cpu` / `--raster-sprites 0` 矛盾 ⇒ exit 2。
- `gpuEligible` 支持：32bpp 后缓冲、GOURAUD、无 alpha test、可映射 blend（含 separate alpha）、stage0 单级 32bpp 纹理、POINT/LINEAR × WRAP/CLAMP/MIRROR、color write mask、TFACTOR、`D3DTSS_TEXCOORDINDEX`。
- 纹理缓存按进程级单调 `serial` + UnlockRect `generation` + 尺寸 + alpha 键控；像素未变不重传。serial 不复用，避免 arena/状态块复用导致读旧像素。
- 真实 boot（最终二进制，80 帧，`--gpu-strict`）：`gpu: draws=146 gpu=146 fallback=0`、`sprite: 776/776 fallback=0 skipped=0`、exit 0；TITLE frames 64–80 wall median 164–202 ms，`DrawPrimitive` median 10.6–11.4 ms。
- 像素：`LIFT_GPU_VERIFY` 同批同状态 16 个 TITLE textured batch 全部 `diff_pixels=0 max_channel_diff=0`；headless CPU↔GPU frame60 为 871/921600，max 1 LSB。
- 剩余下限：Present 中位数 82–108 ms（GPU→guest 3.6 MB 读回 + SDL 上传/呈现），每帧多次 sprite 提交 26–30 ms；TITLE 仍未达 33 ms。
## 验收
1. GPU 覆盖（已完成）：TITLE 段 `--gpu-strict` exit 0，fallback=0；sprite fallback=0；`LIFT_SLOW_DRAW` 不再出现 TITLE 每帧 600–1350 ms。
2. 帧墙钟（未完成）：`--raster gpu --real-time --present window` 下 TITLE ≤33 ms（≥30 fps）；由子单“direct present + sprite batching”继续。
3. 像素：同批 CPU↔GPU 允许 ≤1 LSB；TITLE 当前为 0；不得整层缺失/黑块/方向/alpha 错误。
4. 缺省 `--raster cpu` 逐字节不变；LOGO 不退化；CTest 全绿；`--raster-sprites 0` 不得通过 strict。
5. 守卫：`gpu-mapping` 覆盖 blend/TFACTOR/DISABLE/纹理身份；`gpu-raster` 覆盖 linear/separate alpha 像素；真实 boot 的 strict 是本单收口判据。
## 非目标
- 不做完整 D3D9 纹理 stage 链、16bpp、index buffer、FLAT、alpha test；
- 不改 CPU 光栅的像素判据；GPU 深度的不完全镜像（README §4.2b）另单跟踪。
