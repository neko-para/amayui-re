# 文本门 / 计时器 / 状态字第 29 位：0x196 成功支的外部写入没有服务者

- id: REQ-01M630YKF34EVZPMMBAJVMDDQW
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-adv-pacing
- tags: [text, gate, emulator-cxx]

## 一句话

已收口（2026-10-29）：第 29 位 / 文本计时器 / 每字间隔的**服务者与语义**已取证并落地，
守卫 `emulator-core-adv-pacing`（按毫秒节流）与 `emulator-core-adv-drain`（抽干 + 清位）会红。
第 27 位的**服务者**（`sub_411900`）不在本单 —— 它归 `REQ-01M60CFPK2X36ZCQD21M3E5G7E`。

## 已取证（EA 锚）

- **每字间隔** `Font+0x560`：`0x40967E` 读它 → `0x409685 Sleep`（是**每字间隔毫秒**，不是整段延时）。
  写者 4 处：初始化读配置 `message:MessageSpeed`（`0x417E98` → `0x417EAB`）、`0x74`（`0x41F344`）、
  `0x1B5`（`0x41FF00`，并把同值回存配置）、IAGEService（`0x47626C`）。值 = 当前指令的操作数 1。
- **计时器** `sub_453A60`：通用 0x1C 字节计时器（`+8` 步号 / `+0x14` 起点 / `+0x18` 间隔）；
  推进者 `sub_453B60`：`elapsed <= n × interval` ⇒ 未到期；文本实例 = `Engine+0x691EC`。
- **第 29 位的服务者** = 主循环 `0x41341F test eax,20000000h` → `0x413428 call sub_409400`；
  内部：输入 / 前进键 / 滚轮 ⇒ 清 29 置 27 + 同帧抽干；否则按 `set:DrawMode` 用 `Sleep(间隔)` 或计时器闸门推进一格；
  末格 `0x4096B2` 清第 29 位。**第 29 位不挡脚本**（`0x41342D` 之后仍派发一条指令）。
- **与第 27 位是两套**：27 的服务者 `sub_411900`（清位 `0x411A06`）；两者在前进键 / 外部跳过处互锁。

## 订正（旧记录与字节相反 / 锚错）

- `0x41F5EB` / `0x41F5F8` 不在 `0x82`：属 **`0x7D`**（`sub_41F580`），且那一支**置第 29 位**（`0x41F5E1`）。
- `Engine+0x5EC7C` 不是门位：`0x1BB`（SetTB）写 0 / `0x80000000`，只作第 5 参传给绘制入口（唯一按位读法 = 判 INT_MIN）。
- 跳过支的常量 `2` = **操作数下标**（`sub_41B640(this,2)`），不是门值 / 子状态。

## 已落地（本单）

- `0x6e` / `0x196` 成功支：`message:MessageSpeed > 0` 且第 27 位未置 ⇒ 置第 29 位 + 计时器{间隔, 起点, n=1}；
  否则同帧抽干（不置 29、不建计时器）。
- 文本泵：**每 D 毫秒一格、每帧最多一格**（未到期的帧一格都不画）；末格清 29；按键 / 跳过 ⇒ 抽干 + 清 29。
- 守卫：`emulator-core-adv-pacing`（100 ms 时前 5 帧不画、跨过 100 ms 才画一格；间隔 = 0 ⇒ 当步抽干 + 全画）、
  `emulator-core-adv-drain`（抽干 + 清 29）。两条都做过变异验证（去掉计时器闸门 / 关闭打字分支 ⇒ 各自红）。

## 不属本单（已另行跟踪）

- 第 27 位的服务者（`sub_411900`）与"跳过时置 27"：emulator **不模拟**置 27（该门位没有服务者，置了会卡住主循环）
  —— 见 `REQ-01M60CFPK2X36ZCQD21M3E5G7E` 与 `repo:apps/emulator-cxx/SCOPE.md`。
- 前置支（子系统返回非 0 ⇒ 置 27 + 标志格 `0x7795c`）：同上未模拟。

## 关联

- `data/spec/audit.json` 的 `RES-ADV-TEXT-APPEND-SLOT-BITS` / `RES-ADV-TEXT-APPEND-PRE27`
- `data/spec/chapters/62-adv-text.md` 的 `ADV-TEXT-APPEND`（两档写入 + 文本推进器）
