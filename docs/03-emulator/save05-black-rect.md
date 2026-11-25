# SAVE05 宝藏闪光 → 黑色矩形：复现与归因

> 2026-11-19 / win32 / main `234d887` / `apps/emulator-lift/out/emulator-lift-aot.exe`（RelWithDebInfo）。
> 本文是**一次真跑的取证记录**：命令 + 读数 + 结论 + 未定论。需求单：
> `REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`（sprite 混合因子）/ `REQ-01M7R1AR688T09G2T65GY8SZGG`（影片只送 1 帧）。
> ⛔ 本文不写"修好了没有"（那是需求台账的事）。

## 现象

加载 `saves/SAVE05.DAT`（SC0000 第一个地图中的一个场景），点击宝藏（参考坐标 **740,355**）触发一段闪光特效。
实际画面：**闪光所在位置是一个黑色矩形，整个特效期间一直是黑的**；特效播完后出现交互对话框
（"特效结束"的判据 = 对话框出现，不是黑框消失）。

## 复现

```powershell
cd apps\emulator-lift
# 存档是用户自维护素材：复制到本次私有 appdata，⛔ 不就地写（saves/README.md）
.\out\emulator-lift-aot.exe --game-dir ..\..\links\install `
  --appdata ..\..\.tmp\save05-black\appdata `
  --control-root ..\..\.tmp\save05-black\ctl `
  --control-orphan-timeout 0 --control-idle-timeout 0 `
  --present headless --skip-logo --audio off --frame-log `
  --input-plan ..\..\.tmp\alpha-flash\load05.plan --frame-log --time-limit 1800
```

- `--control-orphan-timeout 0 --control-idle-timeout 0` **必须给**：缺省 10 s 的 orphan 会在两条命令之间
  把端点自己 `quit`（第一次 run 就是这么丢的，`stop: control: quit`）。
- 进入 SAVE05 之后（`SBUNKI.BIN 0x30A4` 停住等点击）：`pause` → `move 640 322` → `resume` → `click 640 322` → `resume`。
  ⚠ 按下与按键电平读点各要跨一次 ⇒ 一次 `resume` 走不完 `up`（见 `REQ-01M7M967MKB65PCY3HAJRTFX9X`）。
- 归因需要 `--log-level info`（缺省 `warn` 会把这些行全藏掉）。⚠ info 档 GDI 字体行刷屏 ⇒ 帧率掉到 ~0.5 fps。

## 读数

| 读数 | 值 | 出处 |
|---|---|---|
| 特效帧 | run2：点击 2827 / 特效 2851–2890 / 对话框 2895；run4：点击 586 / 特效 604–646 / 对话框 648；run5 逐帧 49–50 / MES 51 | `--frame-log` + `emu-ctl sample` |
| 黑框尺寸 | **120×120**，跟着宝藏坐标走 | 帧 49 的 `[d3dx9] End: 合成 1 条 Draw / 14400 个覆盖格`（=120×120） |
| 混合状态 | 帧 49 唯一一条：`State=19⇒0x5`（`SRCBLEND=SRCALPHA`）+ `State=20⇒0x2`（`DESTBLEND=ONE`）；帧 48 的 `DESTBLEND=ONE` 出现 **0** 次 | `--log-level info` 的逐帧切片 |
| 同帧 dshow（**相关读数，不是因果**） | `MovieDecoder open：80x80 fps=30.000 时长=0.800s`；`AddFilter(name="TextureRenderer")`；`tex movie #1 w=120 h=120 nonzero=14 sampled=225`；`completed=true（已送 1 帧）` | 同帧日志 |
| 黑在 guest 画面里 | `screenshot.source=guest-backbuffer` 且 `hash_still_matches=true`（帧 2857 / 2867 每张都是） | `emu-ctl screenshot --json` |
| DDraw | 帧 48/49/50 的 `ddraw` 命中 **0** 次 | 同日志切片 |
| SO033A 覆盖图 | `b1b6 = 0x0 → 0x10`（`raw 0x71C7CBC6 → 0x71C3CBC6`，读回确认）后黑框**仍在**：帧 604 搜索区黑 **11.19%** vs A 组帧 2857 **8.91%** | `emu-ctl global-get/global-set` + 截图 |

## 根因

### ① `ID3DXSprite` 批次丢掉混合因子（黑块的成因）

引擎用**加性**混合（`SRCALPHA / ONE`）画**一个 sprite**（它的源图几乎全黑、只有一点白星）；
本仓的 sprite 合成路只从 `Begin` 的 flags 取一个 bool，混合因子**写死**成 `SrcAlpha / InvSrcAlpha`：

- `src/runtime/d3dx9_sprite.cpp`：`const bool blend = (s.flags & kSpriteAlphaBlend) != 0;` → 只有一句
  `gs_state.blend = blend;`；`rg "kRsSrcBlend|kRsDestBlend" src/runtime/d3dx9_sprite.cpp` = **0 命中**。
- `src/runtime/gpu_raster.h`：缺省 `src_blend = SrcAlpha` / `dst_blend = InvSrcAlpha`。
- fixed-function 路**会读**那两个状态（`src/runtime/d3d9_pipeline.cpp` 的 `kRsSrcBlend` / `kRsDestBlend` 分支）
  ⇒ **两条路的混合语义不一致**。

⇒ 加性下"黑底 = 加 0"不可见；被当成 `InvSrcAlpha` 后黑底（alpha=1）把背景替换成黑 ⇒ 120×120 黑方块，
中间那 14 个非零像素就是画面里那颗白星。

### ② 同帧还看到"影片只送 1 帧"（**与黑块无因果关系**，另行登记）

`已送 1 帧` 与 `0.8 s × 30 fps` 不自洽（`REQ-01M7R1AR688T09G2T65GY8SZGG`，S3）。三个候选（都还没取到读数）：
泵的"现在"取自哪把钟 / 解码器只产出 1 个 sample / `completed` 判据写在首帧 pts 上。
**它对画面有没有可见影响未证**；⛔ 不要把"整段都黑"归因到它 —— 那段已经由 ① 解释（修掉因子后黑底不再出现）。

## 排除项

- **DDraw / DDraw Blt**：三帧切片里 `ddraw` 命中 0 次。
- **emulator HUD / overlay**：本轮没有 `--overlay`（当时也没有 UI 脚本入口）；黑框跟着宝藏走、不在窗口角。
- **窗口合成 / letterbox**：`source=guest-backbuffer` + `hash_still_matches=true` ⇒ 黑在 guest 画面里。
- **SO033A 导航覆盖图**：A/B 见上表 —— 置 bit4 后黑框仍在；且它是**全屏**、由 32 ms 回调门控（静止 ≥3.2 s），
  与"120×120、跟着宝藏"不同。

## 修复后的 e2e（2026-11-19 · `d9739f4` 的 exe）

同一屏同一特效（SAVE05 → 点宝藏 740,355）：

- **120×120 黑块消失、白星可见**：修前帧 2857（`shots/b011.png`，搜索区黑 8.91%）；修后特效帧 3056–3088
  （`fx1..fx6`）没有方块，星直接画在宝藏上（搜索区 v≥200 的亮像素 **447 → 679**）。
- 日志证明**状态没变、像素变了**：帧 59/60 各有 1 条 `State=19⇒0x5` + `State=20⇒0x2` 紧跟
  `[d3dx9] End: 合成 1 条 Draw / 14400 个覆盖格`；特效前帧 0 次；同帧 `tex movie … nonzero=14` 与修前逐字一致。
- ⚠ 同一次 e2e 暴露了**下一条回归**：普通 sprite 变 `ONE/ZERO` 不透明（走廊/对话框发黑）⇒ BUG-35
  （`REQ-01M7R3YPQPNPBZ327H89Q8RTEE`）。⇒ 本节的黑度数字**不能**直接当"整体变好"的判据：
  判据只有"那块黑块在不在"。
- **BUG-35 修好后复测**（dev `a0fd69e`，SAVE05，warn 档 `frames=808`、正常结局）：
  对话框（`MES` 772–808）全屏黑 **1.23%**（回归时 33.70%、修前 1.23%）**且边框花纹回来**；
  特效帧（`FIELD 0x3E118`，742–766）黑 1.14–1.15%、搜索区 1.0–1.18% —— **黑块不在、白星可见、走廊石地可见**。
  截图 `.tmp/sprite-begin-e2e/shots/{f3,f8}.png`。
## 未定论（下一步需要哪条读数）

1. 没有一行日志直接打"这条 sprite 采样的是哪张 texture 的 EA"（120×120 是强相关，不是直读）。
   要钉死：给 `[d3dx9] Draw` / `End` 加一行 opt-in 读数（纹理对象 + src rect）。
2. 影片为什么只送 1 帧：需要泵侧的逐帧 `pts` / `Receive` 次数读数（见 ②）。
3. 该 sprite 源图的 alpha 是 1 还是 0：需要读那张 120×120 纹理的像素（控制层目前没有"读纹理"的原语）。
   两种取值下"因子被丢掉"都是缺陷（一个会盖成黑、一个会把该有的加性算错）。

## 怎么查

```bash
node tools/emu-ctl.mjs sample --json                    # 当前 <script>:<pos>（停住才准）
node tools/emu-ctl.mjs screenshot .tmp/x.bmp --json      # source=guest-backbuffer + hash_still_matches
node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json   # SO033A 覆盖图那位
```

逐帧日志切片 + 黑度统计的临时脚本在 `.tmp/save05-black/`（`analyze.mjs` / `burst.ps1`；不入库）。
