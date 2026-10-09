/**
 * packages/age-format/src/engine/handlers.mjs —— **opcode → handler 的观察记录**（不是模拟器的一部分）
 *
 * ## 为什么它**不在** `apps/emulator` 里
 * `sub_42C5E0` 这类符号是**这份反汇编导出**给的名字，是"哪段代码实现了它"的**证据**，
 * 不是模拟器要用到的语义。模拟器只需要：opcode 号 · 助记符 · argc · 语义。
 * ⇒ 把它放进模拟器 = 让模拟器依赖某一次反汇编的符号命名。
 *
 * 用途：**守卫**拿它回语料复核（现算 opcode→handler 分派表，与本表逐条对账 —— EA 锚的可执行形式）。
 * 台账真源：`Engine+0xA509C/dispatch[0x400]` 一族。
 */

/** opcode → handler（IDA 符号）；纯数值族的那 39 条 */
export const OPCODE_HANDLERS = {
  0x50: 'sub_42C5E0', // add · argc 3
  0x51: 'sub_42C620', // sub · argc 3
  0x52: 'sub_42C660', // mul · argc 3
  0x53: 'sub_42C6A0', // div · argc 3
  0x54: 'sub_42C6E0', // mod · argc 3
  0x55: 'sub_42C720', // mov · argc 2
  0x56: 'sub_42C750', // and · argc 3
  0x57: 'sub_42C790', // or · argc 3
  0x58: 'sub_42C7D0', // sar · argc 3
  0x59: 'sub_42C820', // shl · argc 3
  0x5a: 'sub_42C870', // eq · argc 3
  0x5b: 'sub_42C8C0', // ne · argc 3
  0x5c: 'sub_42C910', // lt · argc 3
  0x5d: 'sub_42C960', // lte · argc 3
  0x5e: 'sub_42C9B0', // gr · argc 3
  0x5f: 'sub_42CA00', // gre · argc 3
  0x60: 'sub_42CA50', // random · argc 2
  0x135: 'sub_42F8B0', // bit-set · argc 2
  0x136: 'sub_42F920', // bit-reset · argc 2
  0x13f: 'sub_42FB40', // check-bit · argc 3
  0x191: 'sub_42CEC0', // (无名) · argc 2
  0x2d0: 'sub_430A50', // (无名) · argc 3
  0x2d1: 'sub_430AB0', // (无名) · argc 3
  0x2d2: 'sub_430B10', // (无名) · argc 3
  0x2d3: 'sub_430B70', // (无名) · argc 3
  0x2d4: 'sub_430BD0', // (无名) · argc 3
  0x2d5: 'sub_430C30', // float-mov · argc 2
  0x2d6: 'sub_430C70', // (无名) · argc 2
  0x2d7: 'sub_430CB0', // (无名) · argc 2
  0x2d9: 'sub_430D60', // (无名) · argc 2
  0x2da: 'sub_426420', // (无名) · argc 8
  0x2db: 'sub_426500', // (无名) · argc 1
  0x2dd: 'sub_434720', // (无名) · argc 2
  0x2df: 'sub_430E30', // (无名) · argc 3
  0x2e0: 'sub_430EA0', // (无名) · argc 3
  0x2e1: 'sub_430F10', // (无名) · argc 3
  0x2e2: 'sub_430F80', // (无名) · argc 3
  0x2e3: 'sub_430FF0', // (无名) · argc 3
  0x2e4: 'sub_431060', // (无名) · argc 3
  // ── ★ 启动链前段（SYSTEM4 的前 70 条会撞到的那批）──
  //    argc 的判据：**handler 体自己写的长度字** `帧+0x5D8F4 = 2·argc+1`（与指令表的 argc 互为独立核验）。
  //    形态：多数是"读操作数 → 写一个引擎标量"或"转发进某个子系统"（见 layout.mts 的 ENGINE_SCALAR_WRITES）。
  0x1a8: 'sub_419690', // dev_ukn · argc 0（体只有协议写 ⇒ no-op）
  0x2f6: 'sub_426820', // (无名) · argc 1（清某个 per-slot 状态 + 子系统调用）
  0x149: 'sub_4229A0', // (无名) · argc 1
  0x21b: 'sub_423C20', // (无名) · argc 1
  0x88: 'sub_41FAB0', // (无名) · argc 1
  0x1ca: 'sub_420240', // (无名) · argc 1（走 vtable 的子系统调用）
  0x252: 'sub_425AB0', // (无名) · argc 1
  0x324: 'sub_41A470', // (无名) · argc 0（子系统调用）
  0x32f: 'sub_4272B0', // (无名) · argc 1
  0x70: 'sub_41ED20', // (无名) · argc 5
  0x71: 'sub_41ED80', // (无名) · argc 1
  0x73: 'sub_41F250', // (无名) · argc 10
  0x78: 'sub_41F450', // (无名) · argc 1
  0x79: 'sub_41F490', // (无名) · argc 3
  0x1c1: 'sub_420070', // (无名) · argc 3
};

/**
 * ## 尚未进 `OPCODE_HANDLERS` 的 handler 符号（★ 本仓**尚未**逐字复核 / 未取 EA 锚）
 *
 * 下面这批是"读实现或复核时要能点名 handler"的 opcode。它们的符号原先只写在
 * `apps/emulator/src/vm/ops.ts` 的注释里 —— 那是**实现层**，不该是"引擎事实"的家
 * （house rule：opcode → handler 符号属于本文件）。
 *
 * ⛔ 它们**不是** `OPCODE_HANDLERS` 的成员：那张表的每一条都由
 * `tools/test/opcodes-handlers.assets.test.mjs` 回**语料**现算对账，而下面这些**还没有**独立复核。
 * ⇒ 复核一条（`pnpm tools opcodes handlers --opcode <opcode>` + 读 `.lst`）就把它搬成上面的数据行。
 * ★ 已经在台账里取了 EA 锚的那些（`0x02` `0x03` `0x05` `0x06` `0x20f` `0x21c` `0x6c` `0x1a2`
 * `0x1a3` `0x1a9` `0x1aa` `0x2de` `0x10c` …）**不在**下面的名单里 —— 台账是那些结论的真源。
 *
 * 控制与流程：
 *   `0x1a7 comment` → `sub_4191B0` · `0x101 poll-input` → `sub_419CC0`
 * 数据搬运 / 数组：
 *   `0x2d8 set-array-to` → `sub_430CF0` · `0x61 lookup-array` → `sub_42CB00` ·
 *   `0x64 copy-local-array` → `sub_42CBE0`
 * 字符串：
 *   `0x192 set-string` → `sub_433660`
 * 对象表（表基址 `Engine+0x15144` 见 `layout.mts`）：
 *   `0x212` → `sub_423A30` · `0x25d` → `sub_425EF0` · `0x213` → `sub_423A80`
 * 启动链前段里 handler ≠ 被转发符号的那两条（被转发符号见 `ops.ts` 的 `PROLOGUE` 表）：
 *   `0x2f8` → `sub_4268D0` · `0x308` → `sub_426B20`
 * 渲染（纹理 / 绘制项 / 颜色 / 网格）与音频 —— **整批**来自旧仓观测索引，未复核：
 *   `0x1f7 detach-texture` → `sub_422BC0` · `0x1f8 create-texture` → `sub_422C20` ·
 *   `0x1f9 set-texture` → `sub_422CB0` · `0x1fa release-texture` → `sub_422E00` ·
 *   `0x1fb draw-texture` → `sub_422E70` · `0x202 set-draw-color` → `sub_4231F0` ·
 *   `0x203 set-draw-color-alpha` → `sub_4232C0` · `0x320 create-mesh` → `sub_432150` ·
 *   `0x322 set-vertex-color` → `sub_426C20` · `0x323 set-vertex-color-alpha` → `sub_426CF0`
 */
