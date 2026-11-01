/** @env pure @kind gate @why 复审修过的关键实现锚被改掉（行为守卫在 CTest，这里给变异测试一个可直接跑的入口） */
/**
 * tools/test/emulator-cxx-review-regressions.test.mjs —— 复审修复的**源码回归锚**
 *
 * 行为断言在 `core_selftest`（`emulator-core-msg-block` / `-scene-relocate` / `-scene-records` /
 * `-misc-cells` / `-misc-ops` / `-adv-pacing`）。`tools/mutate-check.mjs` 只会跑 `node --test <文件>`，
 * 跑不了 CTest ⇒ 这里对**每个修过的关键常量**留一个源码锚，供变异测试证明“改坏它会红”。
 *
 * ⛔ 这不是语义判据；语义由 CTest 判。它只防“修复被静默回退、而 CTest 恰好没有编进当前构建”。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const CASES = [
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    snippet: 'msg_arm_ |= 0x10000;',
    count: 2,
    why: '0x6e / 0x196 成功追加必须置 arm bit16（否则 0x305 整段是死代码）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    snippet: 'std::swap(scene_records_[key_a], scene_records_[key_b]);',
    count: 1,
    why: '0x214 两个键都有记录时必须交换（不是单向覆盖）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    snippet: 'msg_wait_c_ = value;',
    count: 1,
    why: '0x238 写的是消息等待三格的 C（与 0x243 / 0x305 同一组）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    snippet: 'if (created) scene_dirty_ = true;',
    count: 1,
    why: '0x242 只为第一张表建默认记录置脏（建记录分支 → sub_49A300 在 0x49A620 写 +0B5ACh）；写值本身与第二张表都不置脏',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'item.set21e_int0 = g1;',
    count: 1,
    why: '0x220 的 G1 必须同步到 DrawItem（消费者读 DrawItem，不读 SceneRecord）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    snippet: 'if (!rec.key_base_ready()) return;',
    count: 1,
    why: '绘制条目认「基数可用」（脚本 0x213 **或** 0x70 的宿主兜底）；只看 key_base_set 会让 ADV 消息层无字',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.h',
    snippet: 'bool key_base_ready() const { return key_base_set || key_base_fallback; }',
    count: 1,
    why: '「脚本写过」与「f68 可用」是两个格：兜底写过也必须算可用',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'const bool shrink = scale_x >= 1.0 && scale_y >= 1.0;',
    count: 1,
    why: '0x32 缩小走面积加权平均、放大走双线性；判据按采样比分成两支',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'if (msg_wait_c_ != 0) {',
    count: 1,
    why: '0x21c 的谓词 A 必须读 0x238 写的那一对格（起点 0=未盖章、时长）；不读它就会把窗当空气（①③ 的直接成因）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'if (scene_pending_) return true;',
    count: 1,
    why: '0x21c 谓词 A 必须看场景侧的「待处理」闩（`+0B5B4`：0x202 / 0x323 排窗置、窗推进再置、渲染步入清）；不看它 ⇒ 颜色窗没走完就放行（emulator-core-wait-colour-window 红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'scene_pending_ = true;  // `+0B5B4`：这一帧还在推进',
    count: 1,
    why: '窗**推进中**必须把待处理闩重新置起（引擎 sub_4B06D0 的 0x4B326A / 0x4B3CCB），否则渲染步入清掉之后再没人置 ⇒ 窗没走完就放行',
  },
  // ── S4 场景时钟门槛 / 渲染步收尾判据（★ 成对：`tools/mutate-check.mjs` 的 `from` ↔ 本文件的 `snippet`）──
  {
    file: 'apps/emulator-cxx/src/core/vm.h',
    snippet: 'if (!scene_pending_) clear_scene_commands();',
    count: 1,
    why: '渲染步**收尾判据是场景待处理标记**（`60-ops-scene` 的「命令档的前置与出档」：为 0 ⇒ 把**整张**命令表清空）'
      + ' —— 换回「有动画」⇒「排窗但无命令」/「命令在推进但没有动画」两档清错（CTest `emulator-core-selftest` 的 '
      + '`check_scene_clock_gate` ⑨ 当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_io.cpp',
    snippet: 'choice_window_.fallback = -1;',
    count: 1,
    why: '0x93 的清表是七格复位，兜底跳转目标也在里面（0x403F07 [ecx+74ACh] = -1）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'if (down && !advance_input_prev_) advance_input_latch_ = true;',
    count: 1,
    why: '0x72 的等推进按**事件上升沿**锁存（一次按下只放行一次）；按电平读会让长按连发',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'const auto mask = read_int(ops[1]);',
    count: 1,
    why: '0x258 的读序是 2 → 1（先位掩码、再编号）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_engine.cpp',
    snippet: 'config_.set_int("message:ReadTextSkip", read_int(ops[0]));',
    count: 1,
    why: '0x1ca 与 0x1cb 必须读写同一张命名键表',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'if (reveal_count_ > 0) reveal_cursor_ = (reveal_cursor_ + 1) % reveal_count_;',
    count: 1,
    why: '0x1ce 的主循环服务：每拍推进一格必须**取模格数**（格数 0 不推进、不夹成 1）；'
      + '写成裸 +1 会让格号越过格数（CTest emulator-core-text-cell-reveal 当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_numeric.cpp',
    snippet: 'reveal_cursor_ = 0;',
    count: 1,
    why: '0x1ce 的启用支必须把**当前格号归 0**（不是保留上一拍的格号）；停用支不许碰它',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'if (now_ms < start + static_cast<std::uint32_t>(delay) +',
    count: 1,
    why: '窗内判据必须是 `现在 < 起点+延迟+持续时间`（引擎 0x4B17F9 的 `jge`：相等**已到期**）；'
      + '写成 `<=` 会比引擎晚 1 ms（CTest emulator-core-wait-colour-window 的等值用例当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'if (start == 0) start = now_ms;',
    count: 1,
    why: '盖章**不看持续时间**（引擎 0x4B085E 只判"起点为 0"）⇒ 持续时间 ≤ 0 的窗也必须在第一次推进时盖章并立刻到期；'
      + '退回"duration <= 0 直接 return"会让 emulator-core-wait-colour-window 的 dur=0 + delay=200 用例红',
  },
  // ── S1 帧族：行为断言在 CTest `emulator-core-selftest`（`check_frame_shape`），
  //    这里给 `tools/mutate-check.mjs` 一个 node 可跑的源码锚（成对改：from ↔ snippet）──
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'if (f.ret_stack.size() >= kRetStackCapacity) {',
    count: 1,
    why: '返回栈每帧 256（CORE-OPS-FRAME-CALL）：第 257 次压栈必须记诊断并拒绝 —— '
      + '删掉容量检查 ⇒ 栈顶推过 256（check_frame_shape 的容量断言当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'frame.parent = parent_candidate_;',
    count: 1,
    why: '装载器只照抄「父帧候选」格（CORE-OPS-FRAME-LOAD-FRAME 0x40ee05）—— '
      + '改回"读当前帧号"⇒ 泵写的 −10 被冲掉、0x2 的 −10 支永不可达',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'frame.local_main = -1;',
    count: 1,
    why: '新帧记录的两个本地块进入目标 ← −1（CORE-OPS-FRAME-CALL-SCRIPT 的成立条件 / 0x40ee26）—— '
      + '删掉它 ⇒ 复用槽时残留上一次登记的进入目标',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'current_frame_ = static_cast<std::size_t>(suspended_frame_);',
    count: 1,
    why: '0x2 的 −10 支 = 泵挂起帧（CORE-OPS-FRAME-EXIT 0x41a896）：接回挂起帧 —— '
      + '删掉它 ⇒ 泵帧结束后整个执行被当成根结束',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'state_flags_ = engine_state_word_stack_.back();',
    count: 1,
    why: '0x5 的跳转支从**引擎级状态字保存栈**弹一个值写回状态选择字（CORE-OPS-FRAME-RET 0x41aa47）—— '
      + '删掉它 ⇒ 引擎回调结束后门不再关上（状态字回不到挂起前的值）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'state_flags_ &= ~(1u << 26);',
    count: 1,
    why: '第 26 位服务体：输入字非 0 时清第 26 位（CORE-OPS-FRAME-LOCAL-RET-7C 0x412a46）—— '
      + '删掉它 ⇒ 本地块等待永久卡住（输入到了门也开不了）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_value.cpp',
    snippet: 'it = in_ref_bank ? ref_slots_.erase(it) : std::next(it);',
    count: 1,
    why: '0x9 复位三个引用族全局池（CORE-OPS-FRAME-EXIT-SCRIPT 0x428b94 / 0x428bac / 0x428bc4 逐池清 0）—— '
      + '删掉它 ⇒ 引用槽留着旧地址（check_frame_shape 读回旧地址当场红）',
  },
  // ── S2 门模型：行为断言在 CTest `emulator-core-selftest` 的 `check_gate_chain`，
  //    这里给 `tools/mutate-check.mjs` 一个 node 可跑的源码锚（成对改：from ↔ snippet）──
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: '22, 24, 14, 9, 12, 26, 20, 30, 13, 3, 4, 7, 8, 10, 6, 27, 29, 31, 0, 5, 28, 23,',
    count: 1,
    why: '`CORE-VM-STATE` 的**服务链位序表**（22 位，spec 的「检查序」，⛔ 不是位号升序）—— '
      + '删掉第 30 位这一项 ⇒ 逐格揭示的服务体不再被扫到（`check_gate_chain` 的「画一格 + 落到表尾」两半当场红）',
  },
  // ── S3 队列 / 泵：行为断言在 CTest `emulator-core-selftest` 的 `check_command_queue`，
  //    这里给 `tools/mutate-check.mjs` 一个 node 可跑的源码锚（成对改：from ↔ snippet）──
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'if (queue_occupied()) pump_command_queue();',
    count: 1,
    why: '泵帧收场第 ⑤ 步（`CORE-OPS-RUN-BRACKET`）：队列非空 ⇒ **立即再泵一条**（链式）—— '
      + '删掉它 ⇒ 队列停在泵帧收场，剩下的命令不再被取（check_command_queue 的链式那半当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'slot.parent = kPumpParentSentinel;  // 负支由泵直接写，不经过装载器',
    count: 1,
    why: '负命令的目标帧父帧号 ← −10 **由泵直接写**（`CORE-OPS-RUN-BRACKET`）—— '
      + '改回 0 ⇒ 0x2 的 −10 支永不可达',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'frame_mut().ip += 4;',
    count: 1,
    why: '`0x143` 第 4 步自己把指令指针推进 4 字节（`CORE-OPS-EVENT-143`：⛔ 不许留给分派器再加一次）—— '
      + '删掉它 ⇒ 下一条又执行自己（check_command_queue 的 `ip` 只 +4 断言当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'state_flags_ = (state_flags_ & ~(1u << 14)) | (active & (1u << 14));',
    count: 1,
    why: '泵帧收场第 ③ 步：状态字 ← 待恢复副本，但**第 14 位取活动字的值**（不取副本）—— '
      + '改回 `= status_restore_` ⇒ 活动字上的压低位被旧副本盖掉',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'const bool pump = (pump_counter_ & 4u) != 0;',
    count: 1,
    why: '宿主消息泵的节流是「计数器与常量 4 的按位与」（`CORE-VM-LOOP`：泵一次后隔 3 个迭代）—— '
      + '改成别的模数 ⇒ 计数序列不对（check_command_queue 的节流断言当场红）',
  },
  // ── S4 文本槽在用标志：行为断言在 CTest `emulator-core-selftest` 的 `check_gate_chain` ④⑤，
  //    这里给 `tools/mutate-check.mjs` 一个 node 可跑的源码锚（成对改：from ↔ snippet）──
  {
    file: 'apps/emulator-cxx/src/core/vm.cpp',
    snippet: 'if (service_text_layers()) return GateAction::Dispatch;',
    count: 1,
    why: '第 29 位服务体的收场必须读**推进器那一趟算出的「文本槽在用标志」**（`[77984h]`：槽 1..9 '
      + '任一状态字非 0；spec 服务链位序表第 17 行）—— 恒交回迭代头 ⇒ 演示闸层永远派发不出去 '
      + '（check_gate_chain ④⑤ 当场红）',
  },
  // ── S5 时间交付 / 压低接链：行为断言在 CTest `emulator-core-audio-voice`
  //    （`check_time_delivery`），这里给 `tools/mutate-check.mjs` 一个 node 可跑的源码锚
  //    （成对改：`tools/mutate-check.mjs` 的 `from` ↔ 本文件的 `snippet`）──
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'state_flags_ |= (1u << 14);',
    count: 1,
    why: '音乐压低请求必须把**第 14 位写回状态选择字**（`70-vm.md:21` / `0x40716c`）—— '
      + '删掉它 ⇒ 语音推进触发不了压低、第 14 → 9 的交接没有入口（check_time_delivery ④b 当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: 'if (elapsed >= channel.duration) {',
    count: 1,
    why: '音效通道的到期判据是**有符号**「读数 − 起点 ≥ 时长」（`70-vm.md:19`；`70-vm.md:192` 明说音效 / '
      + '语音走有符号）—— 改成无符号比较 ⇒ 起点落在"未来"的通道被判到期（check_time_delivery ② 当场红）',
  },
  // ── S6 门前渲染步的 (c)/(d) 归位：行为断言在 CTest `emulator-core-selftest` 的
  //    `check_scene_clock_gate` ④/④b/④c，这里给 `tools/mutate-check.mjs` 一个 node 可跑的
  //    源码锚（成对改：`tools/mutate-check.mjs` 的 `from` ↔ 本文件的 `snippet`）──
  {
    file: 'apps/emulator-cxx/src/core/vm_timing.cpp',
    snippet: '    if (draw_mode_ != 1) return SceneAdvance::Skipped;                    // (a)',
    count: 1,
    why: '「连盖钟一起跳过」的只有 (a) 绘制模式 / (b) 画面暂停闩 / (c) 0x21b 空转这**三个**'
      + '（`70-vm.md:217`）；(d) 执行区间的暂停在**"只挡推进 / 请求"**组（`:220-225` 第 5 条：'
      + '忙判定在盖钟**之后** ⇒ 钟照盖）。把 (d) 塞回这一行就是 S4 的错归因 ⇒ 暂停期间整个场景时钟'
      + '停走（`check_scene_clock_gate` ④ 与 `check_pause_and_clock` 的时钟断言当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_io.cpp',
    snippet: 'host_now_ms() - mouse_last_ms_',
    count: 1,
    why: '`0xcd` 的时间闸读**现场宿主时刻**（`0x41ACF1 call timeGetTime` / `0x41ACF9 sub edx,[esi+68EF0h]`）'
      + '—— 退回场景时钟对 ⇒ (c) 取消门前盖钟之后 TITLE 的 `0xc8` + `0xcd` 轮询环停摆、输入饿死 '
      + '（`check_scene_clock_gate` ④c 当场红；实测 8 条资产档全红）',
  },
  // ★ 短睡眠的**两个分支**各一个锚（缺哪一半都会红）：谁拥有时钟，就由谁让时间过去。
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_io.cpp',
    snippet: 'sleep_host_(sleep_ms);',
    count: 1,
    why: '`0xc8` 的短睡眠（n < 10）装了**宿主睡眠口**时必须把 n 交给宿主（窗口路径 = 真的 `SDL_DelayPrecise`、'
      + '无窗口路径 = 推进仿真时钟）。处理器体内自己再写 `host_clock_ms_` ⇒ **两个写者互相拨钟**：'
      + 'core 把钟推到未来、宿主下一次采样又拨回真实值 ⇒ 场景时钟往回走，`0x300` 演示闸的停期限'
      + '（无符号 `现在 − 起点 ≥ 期限`）当场成立 ⇒ 示例文本连播（CTest `emulator-core-clock-monotonic` 红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_io.cpp',
    snippet: 'host_clock_ms_ += sleep_ms;',
    count: 1,
    why: '`0xc8` 的短睡眠（n < 10）在**没有**宿主睡眠口时（自测 / 无宿主的虚拟时钟）必须自己让时间前进 '
      + 'n 毫秒（`CORE-OPS-SLEEP-C8`：⛔「短睡眠不得变成 no-op…把 `sleep 1` 当作"什么都不做"会让轮询循环'
      + '以 CPU 全速空转，属于违反本条」）—— no-op 会让 `0xc8` + `0xcd` 的输入环永远等不到自己的时钟'
      + '（`check_scene_clock_gate` ④c 当场红）',
  },
  // ── S7 `0x9` 复位清单的余下项：行为断言在 CTest `emulator-core-selftest` 的
  //    `check_exit_script_reset_scope`，这里给 `tools/mutate-check.mjs` 一个 node 可跑的
  //    源码锚（成对改：`tools/mutate-check.mjs` 的 `from` ↔ 本文件的 `snippet`）──
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'if (engine_reset_hook_registered_) engine_reset_hook();',
    count: 1,
    why: '`0x9` 必须调用**启动时登记的引擎重置钩子**（`CORE-OPS-FRAME-EXIT-SCRIPT`：'
      + '「把当前帧的推进格数写成 1、清状态选择字第 9 位，并复位它自己的一个子系统」；站点 `0x428a71`）—— '
      + '不调 ⇒ 钩子的两件事一次都不做（check_exit_script_reset_scope 的调用计数当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'if ((config_.get_int("set:CreateObject") & 2) != 0 && (config_.get_int("set:AutoFreeTex") & 1) != 0) {',
    count: 1,
    why: '`0x9` 的纹理槽释放判据是**两个配置位同时为 1**（`set:CreateObject` 第 1 位 ∧ '
      + '`set:AutoFreeTex` 第 0 位；站点 `0x428d3d` / `0x428d57`）—— 写成"或"或丢掉任一条件 ⇒ '
      + '不该释放时也释放 / 该释放时不释放（check_exit_script_reset_scope 的正反两半当场红）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_flow.cpp',
    snippet: 'draw_mode_ = config_.get_int("set:DrawMode");',
    count: 1,
    why: '`0x9` 把场景绘制模式格 ← **重读配置** `set:DrawMode`（`CORE-OPS-FRAME-EXIT-SCRIPT`；'
      + '站点 `0x428c34`，与 `0x200` 同口径）—— 沿用构造值 ⇒ 运行期改了配置的复位不生效 '
      + '（check_exit_script_reset_scope 的 DrawMode 断言当场红）',
  },
];

test('★ 复审修过的关键源码锚必须还在（变异测试的入口）', () => {
  const problems = [];
  for (const c of CASES) {
    const text = fs.readFileSync(path.join(REPO_ROOT, c.file), 'utf8');
    const hits = text.split(c.snippet).length - 1;
    if (hits < c.count) {
      problems.push(`${c.file}: 期望至少 ${c.count} 处 \`${c.snippet}\`（实得 ${hits}）—— ${c.why}`);
    }
  }
  assert.deepEqual(problems, [], problems.join('\n'));
});
