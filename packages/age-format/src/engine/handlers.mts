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
