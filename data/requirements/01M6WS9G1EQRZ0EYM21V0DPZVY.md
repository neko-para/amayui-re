# emulator-lift · --anchor-script（以进入某引擎脚本为 0 点）需要先有「引擎脚本可观察事件」接口；本轮只认 at frame

- id: REQ-01M6WS9G1EQRZ0EYM21V0DPZVY
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/input_anchor.cpp#anchor/summary-shows-origin
- done_reason: 机制 + 真机两档都过了：TimelineOrigin 一次性闩锁 + 四单位重定基；采样门控 needsIdentitySamples()；未闩锁 ⇒ origin-missed + exit 5；与 --interaction-graph 解析期互斥。守卫 input-anchor 11 例 / 187 断言（去掉闩锁 ⇒ 63 红），变异 2 条实测红。真机三组 anchor（0 / 0x5207=EBINIT / 0x522D=SETL2DMOC）× window/headless × 各 2 遍：P1 (cur,id)+sample 序同、只闩一次；P2 Δf=1/2/3 与动作序同；P3 无帧停滞折算；不存在的 id 两档都 exit 5 + origin-missed。insns 两档不等且同档两遍差 0.07–1.6% ⇒ 只作诊断。限制：三组 origin.frame 全 0 ⇒ 只能锚 boot 期脚本，已另开单。
- tags: [emulator-lift input script 取证]

## 问题

- `--input-script` 有两档锚：`at frame N`（Present 计数）与 `at guest_ms T`（guest 虚拟时钟）。
- `--present headless`（缺省）两档都确定性；`--present window` 是 **real-time 时钟** ⇒ 毫秒锚不可复现（`repo:apps/emulator-lift/README.md` §4.2c 已写明“那一档用帧”）。
- 于是“等游戏进到 OPTION 再点”只能**手工标定帧号**：先跑一遍看日志找帧，再把 `at frame` 写成那个数。换机器 / 换呈现后端 / 换 CLI 组合，帧号就变。

## 想要的能力

`--anchor-script <id>`：时间线的 0 点 = **首次进入引擎脚本 `<id>`** 的那一刻，而不是 Present 计数。

## 为什么它得先有「引擎脚本可观察事件」接口

lift 执行的是**原机器码**：“进入脚本”是 guest 内部的帧 / 脚本切换，lift 侧没有这个语义事件。现有可用的只有 `repo:apps/emulator-lift/src/runtime/watch.h`（观察一个 guest DWORD 何时被改写；env 门控、调试用）⇒ 它证明“观察 guest 格”这条路存在，但**没有**“进入脚本 N”这个事件，也没有可挂的回调。

两条路都要先取证：① 在引擎的脚本切换点挂钩（先定位并锚 EA）；② 把某个“当前脚本 id 格”变成受监视观察点（先取证那一格是什么）。两者都是**新的可观察接口**，不是给 anchor 加个 CLI 参数。

## 父级裁决（2026-11-08）：本轮**只认 `at frame`**

anchor 是新能力、单独做；⛔ 不要在 `input_script.cpp` 里先做半截（用毫秒近似、或按日志行号猜）。

## 判据（若开做）

- 同一份脚本 + 同一个 anchor id，在 `--present window`（real-time）与 `--present headless` 两档下**触发在同一个 guest 脚本位置**（可观察：脚本 id / 首帧 draw 签名 + 消息序一致）。
- 没进入目标脚本时**响亮失败**（退出码非 0 + 说清“没进入脚本 N”），⛔ 不静默按 0 点跑。
- anchor 只影响时间线**原点**，不改注入通道（仍是 `runtime/input.h` 那一份状态源）。

## 现在绕到了哪里

窗口模式一律 `at frame` + 手工标定帧号；`at guest_ms` 只在 headless 有意义，脚本跨档复用要改时间锚。

`apps/emulator-cxx` 已有同名 `--anchor-script`（`repo:apps/emulator-cxx/src/app.h`）可作**语义参照**，但它是宿主建模的脚本 id，不是 lift 这种“观察 guest”的实现，⛔ 不要照搬。

## 设计已就绪（2026-11-10，只读）+ 父级裁决 12 条

**设计结论**：`TriggerKind::Marker` **不能**直接当 anchor（边沿 vs 首次 / 首样本只播种 / 名字优先 / 重进再发 / 自己 apply 动作 —— 五条都带 `文件:行`），
但它的**观察源**（`sampleIdentity` + 两个采样 EA + 零成本门控 + 响亮失败通道）**可以整块复用** ⇒ 新写一层"闩锁 + 0 点重定基"，
新接口名 `TimelineOrigin`（⛔ 不要叫 `anchor_*`：那名字已被 `after/wait` 的解析期基准占用）。设计全文 `.tmp/reveq/<本单>/design.md`（**不入库**）。

**父级裁决（⛔ 实现时不要重新裁决）**：
1. marker（周期/边沿）与 `TimelineOrigin`（一次性闩锁）**两个概念**分开；⛔ 不新增 `TriggerKind`、⛔ 不叫 `anchor_*`。
2. id = guest 观察到的脚本 id（整数 ≥ 0）；⛔ **不用 `0` 当关闭哨兵**（根脚本 id 就是 0，也不做任何 `!=0` 判断）；参数不出现 = 不启用。
3. **措辞**：一律写"**观察到**进入脚本 X"（0 点只定义在可观测样本上）。
4. **开局就在 X 算命中**（第一个样本即闩锁）。
5. **重进不定基**：0 点只定一次；之后再进入只记 info 日志。
6. **0 点作用于全部时间单位**（frame / insn / guest_ms / wall_ms）；`at wall_ms` 允许，但要在启动时打一行"该锚在 real-time 档不可复现"的警告（⛔ 不因此 exit 2）。
7. **不设超时参数**：跑完仍未闩锁 ⇒ 收尾时**响亮失败**（不必等一个超时窗口）。
8. **失败形态**：复用 `InteractionFailed` 分流（⛔ 不新增 `StopReason`），detail 前缀 `origin-missed:`，**退出码 5**。
9. **互斥**：`--anchor-script` 只能配 `--input-script`（与 `--interaction-graph` 同时给 ⇒ **解析期报错**，因为图有自己的 actuator 与绝对触发）；
   给了 anchor 但脚本里一个触发都没有 ⇒ 用法错误 exit 2。
10. **重定基方式**：闩锁前抑制全部数值触发，闩锁时一次性 `+= origin`（照 `convertFrameTriggersToWall` 的先例）。
11. **闩锁 pending 期间禁用帧停滞折算**（否则会把 frame 触发静默折成墙钟，0 点就假了）。
12. **两档一致性硬判据** = `(cur,id)` 位置 + 动作序 + `Δf` + `!stallFallbackFired()`；⛔ 不要求同 raster 像素 hash；`Δinsn` 只作诊断（`id 0` 见第 2 条，cxx 那边 `!= 0` 的惯例 ⛔ 别照搬）。

## 实现 + 真机两档（2026-11-10，已收口）

- **代码**：`input_script.{h,cpp}` 的 `TimelineOrigin`（id + 四单位）、`noteIdentity`（电平+首次；重进只记 info）、
  `rebasePendingTriggers`（只加数值 kind，跳过 Marker/已应用）、`originMissedDetail`（`origin-missed:`）、`resetOriginState`；
  `machine.{h,cpp}`：门控 → `needsIdentitySamples()`、采样加喂 `cpu_.insns`/`wallMs()`、收尾改判 `InteractionFailed`
  （新 `RunResult::origin_missed`，⛔ 未新增 `StopReason`）、摘要 `origin    : script=… frame=… insns=… guest_ms=… wall_ms=…`（仅 `originSet()`）；
  `main.cpp`：id 解析（拒 `-1`、`0` 合法）、解析期互斥/依赖 ⇒ exit 2、`at wall_ms` 的 real-time 警告（⛔ 不 exit 2）、`origin_missed` ⇒ **exit 5**。
- **守卫**：纯 CTest `input-anchor`（顶层注册）**11 例 / 187 断言**（开局即内 / 经别的脚本后闩锁 / 只闩一次 / id 0 / `origin-missed` /
  与图互斥 / pending 期不折算 / 四单位各一条 / 摘要里 origin 行有·没有各半边）；去掉闩锁 ⇒ **63 条断言红**；变异 2 条（门控退回、摘要行）
  实测红。`ctest` = **72/72**。
- **真机两档（三组 anchor × 两档 × 各 2 遍，共 16 遍 + 3 遍复核）**：P1 ✅ `(cur,id)`/`sample` 序两档全同、只闩锁一次；
  P2 ✅ `Δf=1/2/3`、动作序（`mouse_move 640 360`→`641 361`→`quit`）全同；P3 ✅ 12 遍 0 次帧停滞折算；
  P4 未采（跨 raster 不可比，裁决 12 已定只作诊断）。**未命中 id（0xFFFFFFF0）两档各 2 遍都 exit 5 + `origin-missed:`，摘要无 origin 行。**
- ⚠ **`insns` 不是可复现量**：两档不同、window 同档两遍差 0.07–1.6%（real-time 档 guest 读宿主时钟）⇒ 判据只落 `(cur,id)+frame+sample+动作序`，`insns` 只作诊断。
- ⚠ **限制（已另开单）**：三组 `origin.frame` 全是 0 ⇒ 今天只能锚 **boot 期**脚本；想在"用输入推到某屏之后再锚"做不到
  （pending 期数值触发全抑制是必然的）⇒ 见 `REQ-01M6XVYA68DQWWMQPY795WPN0Q` 下"anchor-script 只能锚 boot 期脚本"那张单。
