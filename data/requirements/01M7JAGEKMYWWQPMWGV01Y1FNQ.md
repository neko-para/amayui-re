# 🐞S2 screenshot 全黑：dumpLastFrame 保留的最后一张 surface 没有像素（同帧 --dump-frame 正常）

- id: REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ
- type: bug
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- repro: apps/emulator-lift/src/runtime/present.cpp#dumpLastFrame
- severity: S2
- tags: [emulator-lift, control-plane, present]

## 现象

`emu-ctl screenshot`（服务端走 `Presenter::dumpLastFrame`）写出来的 BMP **全黑**，而**同一帧**用 `--dump-frame`
（`SurfaceArena::dumpBmp`，present 汇点内当场写）却是正常画面。

## 实测（2026-11-16，macos-arm64 RelWithDebInfo，headless 与 window 各一次）

```
A. headless frame 578:  screenshot → ok, bytes=3686454, hash_still_matches:true
                          check-black → 非黑 0/921600 (0.000%)  lum min/avg/max 0/0.00/0
B. window   frame 544:  screenshot → ok, bytes=3686454, hash_still_matches:false
                          check-black → 非黑 0/921600 (0.000%)  lum max 0
C. 对照 --dump-frame（headless, --max-frames 6）:
                          frame_0001 0.2% · 0002 99.3% · 0003 99.5% · 0004 99.6% · 0005 99.9% · 0006 99.9% 非黑
```
（`node .tmp/check-black.mjs <bmp>` 是只读 BMP 的小工具，`.mjs` 口径；A/B 用 `emu-ctl screenshot`。）

⇒ **不是"headless 没有像素"**（headless 也能有内容，C 就是 headless）；黑的是 **`dumpLastFrame` 那条"保留最后一张"的路径**。

### 追记（同日，探索者的读数；把"恒黑"改成"竞态 + headless 结构性"）

| 场景 | 读数 |
|---|---|
| `--present window --present-hidden`，`screenshot` **重试到"像素非全 0"为止** | **5/7 拿到真图**（LOAD/SAVE 屏的列表、缩略图、"NEW!"），2 张黑 ⇒ 这条路径是**竞态**，⛔ 不是恒坏 |
| `--present headless` | 反复都是黑（与"presenter 没有 sink、保留的那份 surface 拿不到像素"一致） |

⇒ 准确的现象是：**headless 结构性黑；window 档是竞态（同一个暂停点有时有、有时没有）**。
这比"恒黑"更糟的一点是：**`hash_still_matches:true` 与"两拍相同"在两种情况下都能成立** ⇒ 判据 5 完全抓不到。

### 临时规避（已告知探索者，写在这里供别人照用）

- 需要"看画面"⇒ 用 **`--dump-frame '<dir>/frame_{frame}.bmp'`**（那条路径实测正常）+ `node .tmp/check-black.mjs <bmp>` 判非黑比例；
  ⚠ `--max-frames` 要小（3.6 MB/帧）。
- 判据一律用 `data.sample.script.name/pos`、`state_seq`、`fired`/`pending`/`exit`；⛔ 不用 `screenshot` 当证据。
- 若非要 `screenshot`：**重试到非全 0**（并用 `check-black.mjs` 证明），且**在文档里标成"画面读数、非判据"**。

## 判据（能红）

1. `--present headless` 跑到 ≥30 帧后 `screenshot` ⇒ 写出的 BMP **非黑像素 > 50%**（拿 `--dump-frame` 同帧对照，两者内容一致或至少都是画面而不是空）。
2. `--present window` 同样（同一帧的 `--dump-frame` 与 `screenshot` 应当一致）。
3. 修复后判据仍要保住原来那两条：同一暂停点两拍 `shasum` 相同、`hash_still_matches` 的语义不变。

## 猜测（⛔ 未取证，只是缩小范围）

`dumpLastFrame` 读的是"保留下来的最后一张 surface 描述符"（`last_arena_` / `last_surface_` / `last_surface_hash_`），
而 `--dump-frame` 读的是 **present 汇点当时**的那一张。若是 arena 每帧回收/重置了同一块 surface，
那么"留下来的描述符"可能在下一帧就被清空 ⇒ 读到全 0；而 `last_surface_hash_` 也是在**同一个空 buffer** 上重算的，
所以 `hash_still_matches:true` 反而说明不了任何内容问题。

⚠ 这条猜测不要去改实现之前当结论；先看 `presenter_` 里 last_* 的写入点与 arena 的复用策略。

## 对判据 5 的影响（重要）

M0 的判据 5 现在写的是"`paused_at ∈ {present,block}` + `hash_still_matches:true` + 两拍 `shasum` 相同"。
**这三条在全黑 buffer 上也全部成立**（实测 A 就是 `hash_still_matches:true` + 两拍相同）⇒ 判据 5 **只验了稳定性、没验内容**。
修复落地的同一轮必须给判据 5 补一条"非全黑 / 与 `--dump-frame` 同帧一致"。

## 来源

`REQ-01M7HX0CJ202MJ4PZXW80VFA0J` 的 S2 实现后，探索者报"headless 截图全黑"；本条是父级的实测复核（A/B/C 三组）。
