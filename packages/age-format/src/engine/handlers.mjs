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
};
