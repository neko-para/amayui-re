# core spec · 机制地图与首批机制（排除 render/audio/input）

- id: REQ-01M5AJWPM4DXWG3XR6W7JR6XHD
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- done_reason: 机制地图落盘（data/spec/core/README.md：14 个机制 + CORE-VM 五子机制 + FS/BOOT 显式延后）；CORE-ISA 9 条与 CORE-POOL 19 条草稿全部合入并补齐 spec↔EA 映射（bindings 站点、assets 守卫 7 条）；apps/emulator-new 只读 data/spec，用真实脚本跑到第一个未实现 opcode 并报缺口（隔离守卫钉住）
- tags: [engine spec core]

## 背景与目标

core spec = 引擎自身的行为，**排除 render / audio / input** 三个相对独立的模块。
它是指令驱动的那一半：脚本解码、指令迭代、帧、池与值、指令语义、启动入口、容器格式、文件原语。

组织真源：`data/spec/core/README.md`（机制表 / 闭环判据 / 映射规则 / 草稿约定）。
本单管"框架 + 首批机制"；每个机制批次另开子单，不在这张单里堆内容。

## 判据（框架层）

1. 已开批的机制（首批 `CORE-ISA` / `CORE-POOL`，以及 `CORE-VM` 扫描批）都有子单；未开批的机制只存在于机制表（开批时才建单）。
2. 首批机制（`CORE-ISA` / `CORE-POOL`）各有 `.tmp/spec-draft/<机制>/` 草稿：`entries.md` + `links.json`（EA 映射）+ `feedback.md`。
3. 草稿合入后：`pnpm tools spec validate` 绿；每条条目有 `sites` / `functions` / `storage` / `evidence`。
4. 至少一个机制满足 `core/README.md` §4 的闭环判据（闭集全有归宿 + 映射 + 真实程序跑到前沿）。
5. `apps/emulator-new` 能只读 `data/spec` 起来（新项目，不引用 `apps/emulator/src`）。
6. **延后有记录**：`CORE-FS` / `CORE-BOOT` 的条目写 `deferred` + 理由 + 解除条件（不许静默）。

## 拆分纪律

`CORE-VM` 复杂度高（嵌套 while 主循环 + 大量状态标记）：先浅层扫描、再拆子机制，⛔ 不许堆成一组条目。
子单：`REQ-01M5AK56GP9KNDJK4R3XVMJ1E3`（只扫不写条目）。

## 非目标

* 不写 render / audio / input，也不记录它们的 side effect（用户裁决：本质不理解，记录只会造假观测面）。
* 不重写 spec 工具与不变量：先简单处理，框架跑通后整体评估调整。
* 不搬 `apps/emulator` 的代码；只允许读它的架构文档。
* 本轮不实现 `CORE-FS` / `CORE-BOOT`（显式延后，见 `data/spec/core/README.md` §1.1）。

## 怎么查

`data/spec/core/README.md` · `pnpm tools spec describe` · `pnpm tools requirements plan`
