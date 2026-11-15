# 🐞S2 AOT 点 OPTION 响亮停：菜单 switch 的 case 14（0x41B92D，跳表 jpt_41B672 在 .text 内）从未被抬升

- id: REQ-01M7FS59VTS7X4TGVK6Z7MTKJG
- type: bug
- status: open
- parent: REQ-01M7CD1NRK9P5WKR5PJ6GW3067
- repro: docs/03-emulator/defects.md#BUG-24
- severity: S2
- tags: [emulator-lift, lift, closure]

## 症状（用户实测）

`emulator-lift-aot` 跑到 TITLE 后**点 OPTION**，进程停：

```
stop : decode-error @ eip=0041B92D（非正常结局） — EA 0041B92D 既不在派发表里（closure 缺这一行？）
       也不是 shim 桩 EA ⇒ 响亮停（查找 1 次比较；表 33496 项）
```

同样的跑法走到 TITLE 是正常的（`OBS-20261115-06`）；**只有点到 OPTION 这一支才撞上**。

## 已证事实（都可复算）

1. **`0x41B92D` 是菜单命令 switch 的 case 14**：
   ```
   .text:0041B672   jmp  ds:jpt_41B672[edi*4]        ; switch jump
   .text:0041B974   jpt_41B672 dd offset loc_41B679  ; 跳表本体在 .text 内部（共 15 项，case 0–14）
   .text:0041B92D   loc_41B92D:  ; jumptable 0041B672 case 14
   ```
2. **生成物里没有这个块**：`out/aot/lift_aot.ll` 里既没有 `bb_41B92D`，也没有 `switch` 的
   `i32 4307245`（= 0x41B92D）case ⇒ 该 EA **从未被解码/发射**。
3. **同表另外 12 个 case 目标都有 `bb_*` 块**（所以那些分支能跑）；**只有 case 14 没有**。
4. **不是回归**：`apps/emulator-lift/aot/closure.txt` 未被修改（入库件）；
   派发表 `entries=33496`，与今天之前（`OBS-20261115-03`）**完全一致** ⇒ 可派发集合没变。
   今天的改动只动 lowering（S7）与 x87 块内代码，不动闭包/入口集。
5. **范围更广**：语料里 `jmp ds:jpt_*[reg*4]` 型 switch **56 处**。抽 6 张表，
   "case 目标不在闭包根"的比例是 `5/6`、`6/6`、`11/13`、`15/15`、`9/13`、`11/13`。
   ⇒ 菜单/命令分发型代码里，点到没走过的分支都可能撞上。

## 待定的机制（⛔ 别按猜测改，先测）

两种候选，需要一次带仪器的 `lift-gen` 运行来定死：

1. **case 数被读成 14**（对 `cmp <reg>, N` / `ja default` 那段 bounds check 的理解偏一位）⇒ 最后一项落在表外；
2. **逐项读表时有"像不像代码指针"的过滤**把 `0x41B92D` 拒了（它是非 4 字节对齐的 case 目标，
   但同表其它非对齐目标都被接受了 ⇒ 对齐不是判据）。

取证手段：`lift-gen --dump-blocks` 看该分量读到的 case 集合；并把 `0x41B974` 起 15 个 dword
与 PE 字节逐项对照（`.lst` 已给出 13 个不同目标，另两项是重复目标的 `cases 6,7` / `cases 12,13` 写法）。

## 判据

1. **复现命令**（cwd = `apps/emulator-lift`）：
   ```
   out\emulator-lift-aot.exe --skip-logo --present window --max-frames 3000 --game-dir ..\..\dist\install --appdata <空目录>
   ```
   在 TITLE 上点 OPTION ⇒ ⛔ **不再**出现 `decode-error @ 0041B92D`。
2. **机制修在闭包/跳表识别里，不是补数据**：⛔ 不许只把这 11 个 EA 手工塞进 `aot/closure.txt`
   （那修好一个菜单、留 55 张表）。修完要给出"56 张表里有多少 case 目标仍未进闭包"的**前后读数**。
3. **不回归**：`exit 0` / `frames 3000` / `misses 0` / `stop @ 00498AE4` / arena 三元组逐位相同；
   `ctest` 全绿；新增的块数要报（跳表目标会**新增块**，与 D21 那类"只加 entries 不加块"不同）。
4. 守卫：新形态要有**能红**的用例（例如拿一张语料里的真实跳表，断言"它的每个 case 目标都可派发"）。

## 边界

- ⛔ 不要用"把 decode-error 变成静默返回"来消症状 —— 那是把响亮停改成静默错。
- ⛔ 不要按 EA 特判（"这几个 EA 是菜单"），要按**形态**（`jmp ds:[disp32 + reg*4]` + bounds check）。
- 这条属于**闭包发现的缺口**，不是翻译质量；与 `M-lift-2` 的账分开记。
