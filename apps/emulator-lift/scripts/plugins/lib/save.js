// scripts/plugins/lib/save.js —— `loadSave(ctx, slot, options, titleTick)`：**async 载入链**（v2）
//
// ★ **S1（2026-11-25）起本文件只是「库」**：只有具名导出 `loadSave` / `SAVE_DIR_REL` 等，
//   **没有 `export default`、没有 entry 解析**（config 域 `save` / `--plugin-param` / 默认值那套
//   搬到了根入口 `scripts/plugins/load-save.js`，id = `load-save`）。
//   理由（用户口径）：`--plugin` 的 id 只查 `scripts/plugins/` 根（非递归），`lib/` 只被 import
//   ⇒ 「需要单独加载的入口」必须在根上；库永远不是 id 目标。
//
// 合同：`scripts/plugins/plugin.d.ts`（`RunCtx` / `Tick` / `AtWhere`）。
//
// ★ v2 形状（2026-11-28）：**一个 async 函数**，不再是 `{ start, onTrigger }` 的具名链。
//   · 调用方先 `await bootToTitle(t)` 拿到 TITLE 命中那一拍的 `Tick`，再
//     `await loadSave(t, slot, { expect }, titleTick)`；返回值 = **载入终态命中那一拍的 Tick**
//     （场景接续用它做 `hideBattleHelp` 之类的写）。
//   · 每道门 = `await ctx.once(where, { timeoutMs })`；失败文案 / `finish(5)` 语义由 `./gates.js`
//     逐字保留（`load-save: gate timeout (load:save)` …）。`ctx.once` 的门 = **Run 侧入口**
//     （P0 起 task continuation 里可注册）⇒ 动态页链可以"翻一页 → 判 → 再等下一拍"地写。
//   · 写面（click / move）= 刚 `await` 到的那一拍的 `Tick`（`ctx.input` 在 continuation 里是
//     TypeError）。
//
// ★ **前提：调用方保证已经在 TITLE**（2026-11-21 起，TITLE 门不再在本文件里）：
//   `run` 的第一件事就是 `titleTick.input.click(1070,480)`（TITLE 的 LoadData 入口），然后等 SAVE 门。
//   从**冷启动**跑 ⇒ 先 `await bootToTitle(ctx)`（样板 = `../scenario-save07-battle.js`）。
//   ⚠ 不在 TITLE ⇒ 这一下点不进 LoadData ⇒ SAVE 门超时（`load-save: gate timeout (load:save)`）。
//     那是**响亮失败**、也是本文件想要的形态：⛔ 不自己再等一轮 TITLE（那会把"启动没到"与
//     "TITLE 上没点进去"混成同一条超时）。
//
// 链（slot 5 实测；**终态 = 调用方给的 `expect`**，⛔ 本文件不内置场景清单）：
//   click(1070,480) → SAVE [0x1104,0x11C4)
//   → move(300,rowY) → 下一拍 → click(300,rowY) → SBUNKI [0x0B18,0x0C58)
//   → click(640,323) → [expect 里任一脚本 | SBUNKI 0x0AF8]：
//        expect 命中 ⇒ done（返回那一拍的 Tick）
//        SBUNKI [0x0AF8,0x0B18) ⇒ click(690,323) → [expect 里任一脚本] ⇒ done
//   ★ 为什么确认点击等 [0x0B18,0x0C58)：同一张「要读取吗？」对话框，`save07-battle.plan` 头的
//     `--input-plan-trace` 读数 —— 轮询体 [0x09A4,0x09F8) 与读段 [0x0AF8,0x0B18) 里点会被吞
//     （press 只跨 2 个读点），**读段之后** [0x0B18,0x0C58) 点 3/3 被消费。
//     ⇒ 旧窗口 [0x09F8,0x0B18) 的落点多数落在 0x0AF8（读段首）⇒ 点击被吞、链停在那里
//     （正是 `REQ-01M7W2X81F1ASB488EJFQGMQSH` 的签名：`gate timeout (load:field-2)`）。
//   rowY = 行**在页内**的 y（实测表 {0:91,1:150,2:209,3:269,4:328,5:388,6:447,7:507,8:567,9:626}；
//          0–7 的读数来自既有配方（槽 007 = `save07-battle.plan`），9 的读数 = 截图核对
//          `.tmp/fellow-profile/report.md`（槽 009 行中心 ≈ 626），8 = 91+59.5*8=567 与同两张截图逐行吻合）。
//
// ★ 2026-11-22（B 阶段）—— **`loadSave` 自己判断页**（读档界面每页 10 行；槽 010 在**第 2 页**）：
//   页号是**可读的** guest int global `0x138e`（`./guest-globals.js` 的 `readSavePage`；0 基、0..89）。
//   ⇒ `slot` → 目标页 `t = slot / 10`（向下取整）、页内行 `slot % 10`；进屏时先读当前页，再**翻过去**：
//     · 翻页**只用左右大箭头**（左 `(45,357)` = −1 页、右 `(1235,357)` = +1 页，环形 0..89），
//       方向按"更短的那一边"选（`pageStep`）。
//     · 每点一次都**等页号真的变**：每拍读一次页号；位置动了 ⇒ 按新位置再走一步；
//       没动满 `PAGE_STALL_LIMIT` 拍 ⇒ 重试**一次**；再等满一次还没动 ⇒ `finish(5, …)` 响亮失败
//       （⛔ 不假设"点一下就到下一页"、⛔ 不用 `wait`、⛔ 不无限重试）。
//     · 页内行行为**一字不变**（0..7 既有读数照旧；8–9 是新补的行）。
//   ★ 页的读数与取证（2026-11-22 / win32）：
//     进屏 `global-get 0x138e` = 1 且画面 = `LOAD 010..019`；点右箭头 (1235,357) ⇒ 2 且 `020..029`；
//     左箭头 (45,357) 2→1→0→89（`890..899`）⇒ 页是**环形** 0..89（⛔ 不是"到 0 就停"）。
//     ⛔ **顶部页签不是页号的真源**：从页 1 点页签 0（x = 606 + 42k, y = 32），`0x138e` 连 60 个读点都还是 1
//     （读数见 `docs/03-emulator/save-format.md` §翻页，2026-11-22；此前按"页签直接改页号"实现过一版，把 CTest
//     `plugin-runtime` / `plugin-headless` 的 `load-save-e2e` 钉在 `page not reached (target 0, still 1)`）。
//     ⇒ 顶部那排只影响"显示哪十年组"，⛔ 不要拿它翻页。详见 `docs/03-emulator/save-format.md`
//     §加载界面的列表与翻页。
//
// ★ 2026-11-25 —— **翻页等待的读点预算 60 太小**（11-24 的「3 次 2 次 `page not reached`」根因）：
//   实测（win32 / dev / `out/emulator-lift-aot.exe`（2026-11-24 23:28）/ `--present headless`，
//   私有 appdata 从 `saves/` 复制；5/5 run）：点左箭头 `(45,357)` 之后，页号 `0x138e` 在
//   **76–77 个读点 / ≈210 ms** 才变（变的那一拍 `pos=0x6EF4` / `0x6F44` = `SAVE.BIN` 的翻页处理段；
//   读点密度 ≈ **366 个/s**）。
//   ⇒ 旧 `PAGE_STALL_LIMIT = 60`（≈164 ms ≈ 1.5 帧）**比点击自身还短**：插件点击的抬起要跨过
//   按下之后的**两个完整 guest 回调**（闸门②′，`PointerCore::kCallbackFramesBeforeRelease`；
//   2026-11-22 `f7ec239` 接进插件路）。
//   · 正常时：`turn` 先撞满 60 ⇒ 发一次"重试点击"；第一下在 76 落地 ⇒ `turnr` 接住（"看起来能用"）；
//   · 第一下被吞时：重试点击**自己**又要 76 个读点才落地，而 `turnr` 预算只有 60 ⇒ 重试**结构上
//     救不回来** ⇒ `finish(5, "page not reached (target 0, still 1)")`
//     （= `REQ-01M86VRRTPY8W9RP1SFDT0N200` 的签名）。此外那记多余的重试点击会在页号已变之后落地，
//     把页**又**翻一格（0 → 89）⇒ 之后按页内行 y 点下去落到空槽（= `load:sbunki` 超时的另一条路）。
//   ⇒ 修法（v1）：预算改 **1200 个读点**（≈2.6× 实测上界 461 读点）⇒ 正常时 `turn` 自己就看到页号变、
//   **不产生**重试点击；只有点击真丢了才重试。另在点行**之前**再核一次页号（不符就回页链、⛔ 不点行）。
//   （★ v2 的采样单位变成 "SAVE 回调"，1200 这个数字**不适用** —— 见下面 §v2 采样频率与
//   `PAGE_STALL_LIMIT` 处的重算：205 拍 ≈ 同一墙钟 ≈3.31 s。）
//   ★ **真游戏 A/B（同一份 `saves/`、同一个 exe）**：把第一下箭头点击**故意点空**（= 模拟"第一下被吞"）⇒
//     旧 60：`run=finished exit=5 reason=load-save: page not reached (target 0, still 1)`（calls=126）；
//     新 1200：`page 0 reached` → `reached REIGN.BIN`（calls=723）⇒ 重试窗口是这条链唯一的救命面。
//   ⛔ 没有放宽任何门（判据仍是"页号真的等于目标页"）；⛔ 没改 `saves/`；⛔ 没有用 `--input-plan`。
//   ★ v2 的采样频率（2026-11-28；**复审 S1 订正**）：v1 的预算单位是**读点**（366/s）；v2 用
//     `{script:"SAVE.BIN", pos:SAVE_INPUT_CALLBACK_LO}` 采样 = **每个 SAVE 回调一拍**（回调体入口
//     指令每回调执行一次）。先量**单位的速率**（`.tmp/plugin-v2/p1fix-save-rate.js`：`ctx.at` 计数 +
//     `ctx.delay(3000)`）：**61.9 个回调/s**（3/3 run：62.00 / 62.00 / 61.67）；同窗口 `--frame-log`
//     ≈ 26 fps ⇒ **1 个回调 ≈ 2.4 帧**（复审"1 拍 ≈ 1 帧 / 18 fps"的估法不成立）。
//     ⇒ `PAGE_STALL_LIMIT` **不能沿用 1200**，也不能按"18 fps ⇒ 60"（那只有 ≈0.97 s = v1 的 1/3）；
//     按同一墙钟目标重算得 **205**（≈3.31 s；算法与余量见常量定义处）。
//     ⛔ 不要用 `{script:"SAVE.BIN"}`（= 下一条**指令**）当采样：频率高一个量级 ⇒ 预算会在
//     ~10 ms 内烧完（远小于实测 210 ms）⇒ 正常点击会被误判成"丢了"。
//
// ★ 2026-11-25 —— **缺 `SAVE<NN>.DAT` 在第一个 click 之前就报**（本轮的第二个修复）：
//   以前缺文件要等 SAVE 屏点行不弹确认框 ⇒ 30 s 门超时（`load-save: gate timeout (load:sbunki)`），
//   症状和"点击被吞"混在一起。现在 `run(ctx, …)` 先 `ctx.appdata.stat(<saveDir>/SAVE<NN>.DAT)`：
//   缺 ⇒ `ctx.finish(5, "load-save: save file missing: <绝对路径> (slot N)")` + 结束，
//   **一个 click 都不发**。只查 `.DAT`（`.STH` 缺失不阻止读档；`SAVE.DAT` 不是槽索引）；
//   槽 0..99（≥100 的字形未取证 ⇒ 跳过 + warn）；没有 `--appdata` / stat 报 denied/io ⇒ 跳过 + warn
//   （⛔ 不误杀）。★ 2026-11-25 订正：`saveDir` **目录不存在**（全新 / 空 appdata）也算缺文件 ⇒
//   finish(5)（旧版把它当"存档在安装树"静默跳过 ⇒ 本轮要消除的 30 s 超时又回来了）；
//   只有 `escape` / `invalid_path` / `denied` / `io` 才跳过。
//   有文件 ⇒ 下面整条链**逐行不变**。
//
// ★ 2026-11-21（A 阶段）—— **两个接口，各管一件事**：
//   · `options.expect: string[]`（**必填**）：载入完成后落在哪个脚本，由**调用方按预期给** ——
//     ADV = `SC*` / `SG*` / `SP*`（如 `SC0000.BIN`）、战斗 = `BTL.BIN`、地图 = `FIELD.BIN`（可能还有别的名字）。
//     照宿主读数拼写、**带 `.BIN`**。终态 = 每个 `expect` 一条 `at` 臂（`{script}` = 该脚本下一条指令）
//     + 一条 SBUNKI 第二道确认臂；第一条命中的臂赢（`./gates.js` 的 `waitFirst`）。
//     缺 / 空数组 / 元素不是非空字符串 ⇒ **调用时 throw**（消息里给出三类别名，⛔ 不猜）。
//   · ★ v2 删掉 `options.onLoaded`：场景现在就是 `loadSave` 之后的顺序 `await`（调用方拿返回的
//     Tick 自己做下一步），不需要回调把控制权交回去。
//
// ★ **本文件没有 default 导出**（S1）：`--plugin <本文件>` 不再是合法用法（id 也不查 `lib/`）。
//   要"直接当插件跑"请用根入口 `scripts/plugins/load-save.js`（`--plugin load-save` 或给路径）；
//   它负责 config > `--plugin-param` > 默认 的那套取值，再调本文件的 `loadSave`。
// 库消费者请用具名 `export async function loadSave(ctx, slot, { expect, … }, titleTick)`。
//   ★ 本文件只 import **同目录的兄弟文件**（`./gates.js` / `./guest-globals.js` / `./script-positions.js`）：
//   宿主 QJS 的 import 根 = **入口文件所在目录** ⇒ 入口在根上时，根入口用 `./lib/…` import 本文件，
//   本文件再用同目录 `./x.js` 找兄弟（向下 import 允许；⛔ 向上 `..` 越根会被 QJS 拒）。
//
// ★ 失败面（全部响亮，⛔ 不静默）：
//   ① slot 非整数 / 不在 0–`MAX_SLOT`（899 = 90 页 × 10 行，界面能选到的范围）⇒ **调用时** throw
//      （根入口在 `start` 里调 ⇒ Run fail + 点名）；⚠ 槽 ≥100 的文件名字形**未取证**；
//   ①' `expect` 缺 / 空 / 元素不是非空字符串 ⇒ **调用时** throw（消息给出 ADV / 战斗 / 地图三类别名）；
//   ② 未知 / 等不到的状态 ⇒ 现在没有"未知事件"这回事（v2 没有 `onTrigger`），门等不到 = `once` 超时；
//   ③ 门超时（`timeoutMs`）⇒ `ctx.finish(5, "load-save: gate timeout …")` + 结束（`gates.js`）。
//      ⛔ 不用 `ctx.quit`（那会把归因改成 `plugin: quit`）/ ⛔ 不用 `ctx.run`（C5）。
// ★ 本函数**不依赖**模块级进度状态：所有进度都在这一次 async 调用的局部变量里（v2 的 task 就是
//   那条链本身）。

// ★ 位置常量是**同目录的兄弟模块**（`./script-positions.js` / `./guest-globals.js`）。
//   本文件从"已经在 TITLE"开始 ⇒ TITLE 门在 `./boot-to-title.js` 里（由调用方组合），不在本文件。
import { awaitPosition, awaitWhere, waitFirst } from "./gates.js";
import { readSavePage } from "./guest-globals.js";
import {
    SAVE_INPUT_CALLBACK_HI,
    SAVE_INPUT_CALLBACK_LO,
    SBUNKI_CONFIRM_CLICK_HI,
    SBUNKI_CONFIRM_CLICK_LO,
    SBUNKI_CONFIRM_READ_SEG_HI,
    SBUNKI_CONFIRM_READ_SEG_LO,
} from "./script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Tick} Tick */
/** @typedef {import("@amayui/lift-plugin").AtWhere} AtWhere */
/** @typedef {{ where: AtWhere, tag: string }} LoadArm */
/** @typedef {{ tick: Tick, tag: string }} LoadHit */
/**
 * `loadSave` 的选项。`expect` 在**运行期**必填（缺 / 空 / 元素不是非空字符串 ⇒ 调用时 throw），
 * 这里标 `?` 只是为了让"省略 options / 省略 expect"能通过类型检查后由那段运行期校验响亮拒绝。
 *
 * @typedef {object} LoadSaveOptions
 * @property {string[]} [expect] 载入完成后**可以算落点**的脚本名表（必填，见上）。
 * @property {number} [timeoutMs] 每道门的超时；缺省 30000 ms。
 * @property {boolean} [precheck] 缺 `SAVE<NN>.DAT` 的前置检查；缺省 `true`。`false` = **跳过**
 *   （只打一行 info）—— 给"存档确实不在 appdata（`set:UseAppDataFolder != 1`）"的宿主的逃生口；
 *   ⛔ 不是让"真缺文件"静默通过的开关。
 * @property {string} [saveDir] 存档目录，**相对 appdata 根**（缺省 `SAVE_DIR_REL`）。
 *   只影响前置检查；给的值必须是非空相对路径（绝对路径 / `..` / NUL ⇒ 调用时 throw）。
 */

// ★ 脚本身份 = **marker 读数**（`PluginReadings::sample` ⇒ `MarkerSampler::resolveName`），实际是
//   文件名 `TITLE.BIN` / `SAVE.BIN` / …（实测：`--plugin` 探针在 headless boot 读到
//   `SYSTEM4.BIN` / `LOADCONFIG.BIN` / `TITLE.BIN`）。C1 的 `afterScript` / `afterPosition` 是
//   **精确 `==`**；v2 的 `ctx.at` / `ctx.once` 的 `script` 匹配同一个精确相等（⛔ 不做
//   `await script` 那层的"两侧剥 `.BIN` + ASCII 大小写不敏感"）
//   ⇒ 链里的名字必须带 `.BIN`，否则门永远不命中（纯假读数用例也要照这个拼写）。
const SCRIPT_SAVE = "SAVE.BIN";
const SCRIPT_SBUNKI = "SBUNKI.BIN";

// 半开区间（实测；与 `scripts/recipes/save05-move.plan` / `save07-battle.plan` 逐字同源）。
// ★ 数值只在 `./script-positions.js` 里写一次（本文件 ⛔ 不写位置字面量；守卫
//   `tools/test/script-map.test.mjs` 会把"裸写 0x…"判红）。名字里带上用途：
//   · `SAVE_INPUT_CALLBACK_*` = SAVE 的回调体（★ 保存屏与加载屏共用 ⇒ 位置区分不了两屏）；
//   · `SBUNKI_CONFIRM_CLICK_*` = 确认框**读段之后**的窗口（点击在这里 3/3 被消费）；
//   · `SBUNKI_CONFIRM_READ_SEG_*` = 确认框的**读输入段**（在里面点会被吞 ⇒ 只当第二道确认的回环门）。
//   ★ 改名映射（旧名 → 新名，值不变；旧名已删，⛔ 别留两套）：
//     旧 `SBUNKI_CLICK_LO/HI = [0x0B18,0x0C58)`   → `SBUNKI_CONFIRM_CLICK_LO/HI`（**点击窗口**）
//     旧 `SBUNKI_CONFIRM_LO/HI = [0x0AF8,0x0B18)` → `SBUNKI_CONFIRM_READ_SEG_LO/HI`（**读段**）
//   ★ 另一条更老的"点击窗口" `[0x09F8,0x0B18)`（= 轮询体 + 读段；落点多数在读段首 ⇒ 点击被吞）
//     已被 `SBUNKI_CONFIRM_CLICK_*` 取代，**不在常量表里**（`<待核>` 且无 lib 消费方；
//     3 份 `.plan` 仍以字面量用它 —— 清单见 `.tmp/script-map/provenance.md` §4-1）。

// 坐标（1280×720 参考系；实测）。
const LOAD_MENU_X = 1070;
const LOAD_MENU_Y = 480;
const SLOT_X = 300;
const CONFIRM_X = 640;
const CONFIRM_Y = 323;
const CONFIRM2_X = 690;
const CONFIRM2_Y = 323;

// 页机制（2026-11-22 实测；读数与命令见文件头）。
const SAVE_PAGE_ROWS = 10; // 每页 10 行 ⇒ 槽 = 页 * 10 + 行
const SAVE_PAGES = 90; // 页号 0..89（环形：0 的上一页 = 89）
// 翻页只用**左右大箭头**（±1 页）。★ 顶部页签（x = 606 + 42k, y = 32）实测**改不动**页号：
//   从页 1 点页签 0，`global-get 0x138e` 连 60 个读点都还是 1（读数见 `save-format.md` §翻页；
//   同一读数让 CTest `plugin-*` 之前在 `load-save: page not reached` 上红过）⇒ 页签不是页号的真源，⛔ 不用它。
const PAGE_PREV_X = 45; // 左侧大箭头 = 上一页（页号 −1，环形）
const PAGE_NEXT_X = 1235; // 右侧大箭头 = 下一页（页号 +1，环形）
const PAGE_ARROW_Y = 357;
// 点一次箭头后等"页号真的变了"的拍数上限（v2：一拍 = SAVE 回调体入口执行一次）。
//
// ★ 2026-11-25 v1 时期 60 → 1200（**回归修复**；读数见文件头 §2026-11-25）：
//   实测一次箭头点击从"提交"到"页号变"= **76–77 个读点 / ≈210 ms**（5/5 次无干扰的探针 run：
//   76,77,76,76,77；页号变的那一拍落在 `SAVE.BIN` 的翻页处理段 `pos=0x6EF4` / `0x6F44`），
//   读点密度 ≈ **366 个/s** ⇒ **60 个读点 ≈ 164 ms ≈ 1.5 帧**，而点击的抬起要等按下之后的
//   **两个完整 guest 回调**（`PointerCore::kCallbackFramesBeforeRelease`，闸门②′）⇒ 旧值**比点击自身还短**。
//   真 run 里的落地延迟还**在抖**：12 次 `save.js` run 的页链回调数 = 85…461（≈0.2–1.3 s，
//   抖动来自"2 个回调"的帧长本身）⇒ 上界按 **461 个读点**取。
//   ⛔ 旧注释写的"一个读点 ≈ 一拍输入轮询 ⇒ 60 拍 ≈ 1 s"是错的（实测 60 拍 ≈ 0.16 s）。
//   1200 这个数字当时（v1）的余量 = 1200 / 461 ≈ **2.6× 实测上界**，墙钟 ≈ **3.3 s**（1200/366）。
//
// ★ **2026-11-28 P1 复审 S1：单位变了，数字必须跟着重算**。v2 的采样 = SAVE 回调（不是读点）：
//   沿用 1200 就是错的数量级。★ 复审报告用 "18 fps ⇒ 1 拍 ≈ 1 帧" 估出 60；本片**直接量了单位的
//   速率**（宿主 API 探针，`.tmp/plugin-v2/p1fix-save-rate.js`：进读档屏后装
//   `ctx.at({script:"SAVE.BIN", pos:SAVE_INPUT_CALLBACK_LO})` 计数 + `ctx.delay(3000)`）：
//     · **SAVE 回调 ≈ 61.9 个/s**（3/3 run：62.00 / 62.00 / 61.67；`hits=186 / 3 s`）；
//     · 同一窗口 `--frame-log` 是 262 帧 / 10 s ≈ 26 fps ⇒ **1 个回调 ≈ 2.4 帧**（不是 1 帧/回调）
//       ⇒ "18 fps ⇒ 60 拍" 会把窗压到 ≈0.97 s（v1 的 1/3），**不满足**"同一墙钟目标"。
//   ⇒ 按 v1 的同一墙钟目标重算（v1 的换算因子仍是既有的 366 读点/s）：
//     · 目标窗 = 1200 读点 / 366 读点每秒 = **3.279 s**；× 61.9 回调每秒 ≈ **203 拍**；
//     · v1 的上界 = 461 读点 / 366 = 1.260 s ⇒ **78 拍**；203 / 78 ≈ **2.6×**（与 v1 同一个余量）；
//     · ⇒ 取 **205 拍**：墙钟 ≈ 205 / 61.9 = **3.31 s**（v1 3.28 s，+1.0%），余量 205/78 ≈ **2.63×**。
//   正常点击只需 ≈3 拍（slot 7 实测：9 次箭头点击共 28 个页链采样）⇒ 正常时 `turn` 阶段自己就看到
//   页号变（**不产生**重试点击，也就不会有多余的第 2 次翻页）；只有点击真丢了才走重试
//   （最坏 ≈2×205 拍 ≈ 6.62 s 后响亮失败，与 v1 的 2×1200/366 ≈ 6.56 s 同量级）。
//   ⚠ 这个数绑的是"本机 61.9 回调/s 下的 ≈3.3 s"；换机器时**两个方向**都按计数放大/缩小（与 v1 的
//     1200 同一性质：计数固定、墙钟随机器变）。
//   ★ 为什么不用墙钟 deadline（`ctx.delay` 兜底）：本片按父级裁决只做**计数换算**；墙钟版需要
//     "计数与 deadline 谁先到"的第二套判据（且 `delay` 在正常路径会留 pending ⇒ 影响 Run 收口），
//     属另一张单。这里先把**单位错误**消掉，速率 / 算法 / 余量都写在上面。
const PAGE_STALL_LIMIT = 205;
// 点击被吞时的第二次机会（同样是 PAGE_STALL_LIMIT 拍）。
const PAGE_RETRY_PHASE = "turnr";

// 行 y 的**实测表**（⛔ 不用 91 + 59.5*n 之类的公式猜；0–7 与 9 都有读数，8 与截图逐行吻合）。
/** @type {Readonly<Record<number, number | undefined>>} */
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447, 7: 507, 8: 567, 9: 626 };

// 槽号上限：读档界面 = 90 页 × 10 行（页签条实测到"890"，页号 0..89 环形）⇒ 手选槽 0..899。
// ⚠ 槽 ≥ 100 的**文件名字形**未取证（`saves/` 只到 `SAVE99`；`%2.2d` 遇 3 位数会照写）⇒ 本库只按
//   "界面能选到的槽"放行，⛔ 不声称 SAVE100.DAT 一定能打开。
const MAX_SLOT = 90 * SAVE_PAGE_ROWS - 1;

// ---------------------------------------------------------------------------------------
// 存档文件前置检查（2026-11-25）——缺 `SAVE<NN>.DAT` 时**在第一个 click 之前** `finish(5)`。
//
// ★ 为什么放在本库（而不是某个 scenario）：`../scenario-fellow-switch.js` 也直接调 `loadSave`；
//   放场景里会漏掉直接消费者。`run` 的第一件事 = 这个检查（⛔ 一个 click 都不发）。
// ★ 为什么只查 `SAVE<NN>.DAT`：
//   · `.STH` 缺失**不阻止读档**（`docs/03-emulator/save-format.md` §1：缩略图读侧失败返回 1）；
//   · `SAVE.DAT` 不是槽索引（它是脚本键持久化池；缺它最多让页号回 0）。
// ★ **存档目录是本标题的实测布局，不是引擎合同**：宿主不知道 title / `set:SavePath`
//   （`set:UseAppDataFolder != 1` 时存档根本不在 appdata）⇒ 标题名只在这里写一次，
//   `loadSave(..., { saveDir })` 允许调用方覆盖（相对 appdata 根）。
// ---------------------------------------------------------------------------------------
export const SAVE_DIR_REL = "Local/Eushully/天結いキャッスルマイスター/SAVE";
// 文件名形状**已取证**的槽上界（两位 `%2.2d`）：0..99。`slot > 99` ⇒ 跳过检查 + warn。
const SAVE_FILE_PRECHECK_MAX_SLOT = 99;

/**
 * `SAVE<NN>.DAT` 的 `%2.2d` 零填充（槽 7 ⇒ `SAVE07.DAT`）。
 *
 * @param {number} n 槽号。
 * @returns {string}
 */
function pad2(n) {
    return String(n).padStart(2, "0");
}

// ★ 沙箱里**没有 RegExp**（`script_quickjs.cpp` 的 `JS_NewContextRaw` + BaseObjects）⇒ 本文件
//   一律用字符 / `split` / `endsWith` 处理路径，⛔ 不写正则字面量。
/**
 * ASCII 字母判定（`C:` 这种盘符前缀；⛔ 不用正则）。
 *
 * @param {string} ch 单个字符。
 * @returns {boolean}
 */
function isAsciiLetter(ch) {
    return (ch >= "A" && ch <= "Z") || (ch >= "a" && ch <= "z");
}

/**
 * 去掉末尾的 `/` / `\`（保留 "" / "C:" 这种形状；⛔ 不用正则）。
 *
 * @param {string} text 待去尾的路径。
 * @returns {string}
 */
function stripTrailingSeparators(text) {
    let out = text;
    while (out.length > 1 && (out.endsWith("/") || out.endsWith("\\"))) out = out.slice(0, -1);
    return out;
}

/**
 * 把 `saveDir` 选项归一化成一个**相对 appdata 根**的路径；非法 ⇒ 调用时 throw。
 *
 * @param {string | undefined} value
 * @returns {string}
 */
function normalizeSaveDir(value) {
    if (value === undefined) return SAVE_DIR_REL;
    if (typeof value !== "string" || value.trim() === "") {
        throw new Error("loadSave: options.saveDir 必须是非空字符串（相对 appdata 根）");
    }
    let dir = stripTrailingSeparators(value.trim().split("\\").join("/"));
    if (dir === "" || dir === ".") return ".";
    // 绝对路径（`C:` / `C:\` / `/` / `\`）与 `..` 段都是编程错误 ⇒ 调用时响亮拒绝。
    if ((dir.length >= 2 && isAsciiLetter(dir[0]) && dir[1] === ":") || dir[0] === "/" || dir[0] === "\\") {
        throw new Error(`loadSave: options.saveDir 必须是相对 appdata 根的路径（收到 '${value}'）`);
    }
    const parts = dir.split("/");
    for (let i = 0; i < parts.length; i += 1) {
        if (parts[i] === "..") {
            throw new Error(`loadSave: options.saveDir 不许含 '..' 段（收到 '${value}'）`);
        }
    }
    for (let i = 0; i < dir.length; i += 1) {
        const code = dir.charCodeAt(i);
        if (code === 0 || code < 32) {
            throw new Error("loadSave: options.saveDir 不许含 NUL / 控制字符");
        }
    }
    return dir;
}

/**
 * 失败消息里的绝对路径（`ctx.appdata.path` 的分隔符口径；不可用 ⇒ 退回相对路径）。
 *
 * @param {RunCtx["appdata"]} appdata
 * @param {string} rel
 * @returns {string}
 */
function displayPath(appdata, rel) {
    const root = typeof appdata.path === "string" ? appdata.path : "";
    if (root.length === 0) return rel;
    const sep = root.includes("\\") ? "\\" : "/";
    return stripTrailingSeparators(root) + sep + rel.split("/").join(sep);
}

/**
 * 前置检查：缺 `SAVE<NN>.DAT` ⇒ `finish(5)` 并返回 false（调用方 **一个 click 都不发**）。
 * 跳过（返回 true，照常读档）的情况都必须**打一行 warn**（⛔ 不静默）：
 *   ① `ctx.appdata` 不存在 / `available=false`（没有 `--appdata`）；
 *   ② `slot > 99`（文件名字形未取证）；
 *   ③ stat 报 `denied` / `io`（无法证明"缺文件"），或存档目录 stat 报 `escape` / `invalid_path`
 *      （路径非法 / 符号链接逃逸）；
 *      ★ **目录不存在不跳过**（2026-11-25 订正）：那是"全新 / 空 appdata"，按缺文件 finish(5)。
 *
 * @param {RunCtx} ctx
 * @param {number} slot
 * @param {string} saveDir 相对 appdata 根
 * @returns {boolean} true = 继续读档；false = 已经 finish(5)，调用方必须结束
 */
function precheckSaveFile(ctx, slot, saveDir) {
    const appdata = ctx.appdata;
    if (appdata === undefined || appdata === null || appdata.available !== true) {
        log.warn(`load-save: appdata 不可用 ⇒ 跳过存档文件前置检查（slot ${slot}）`);
        return true;
    }
    if (slot > SAVE_FILE_PRECHECK_MAX_SLOT) {
        log.warn(
            `load-save: slot ${slot} > ${SAVE_FILE_PRECHECK_MAX_SLOT} 的 SAVE<NN>.DAT 文件名字形未取证` +
                " ⇒ 跳过存档文件前置检查",
        );
        return true;
    }
    const rel = `${saveDir}/SAVE${pad2(slot)}.DAT`;
    const st = appdata.stat(rel);
    if (st === undefined || st === null || typeof st !== "object") {
        log.warn(`load-save: appdata.stat(${rel}) 没有返回结果对象 ⇒ 跳过前置检查`);
        return true;
    }
    if (!st.exists) {
        // ★ 无法证明"缺文件"的读法一律跳过：denied / io 可能是环境问题，no_appdata 是配置晚了。
        const error = typeof st.error === "string" ? st.error : "not_found";
        if (error === "denied" || error === "io" || error === "no_appdata") {
            log.warn(`load-save: stat ${rel} 失败（${error}）⇒ 跳过存档文件前置检查`);
            return true;
        }
        // ★ 再查一次**存档目录**：只有"目录存在但这一槽没有文件"才是缺文件；
        //   `saveDir === "."` = appdata 根本身（`available=true` 已证明根可用）⇒ 不用 stat 它。
        //   ★ 2026-11-25 订正（复审 I5）：`stat(saveDir)` 的 `not_found` = **目录真的不存在**
        //     （全新 / 空 appdata）⇒ 也是缺文件、按设计 finish(5)。只有 `escape` / `invalid_path`
        //     （路径非法 / 符号链接逃逸）与 `denied` / `io`（无法证明）才跳过 + warn。
        let dirOk = false;
        let dirError = "";
        if (saveDir === ".") {
            dirOk = true;
        } else {
            const dirStat = appdata.stat(saveDir);
            dirOk =
                dirStat !== null &&
                dirStat !== undefined &&
                dirStat.exists === true &&
                dirStat.isDir === true;
            if (!dirOk && dirStat !== null && dirStat !== undefined && typeof dirStat.error === "string") {
                dirError = dirStat.error;
            }
        }
        if (!dirOk) {
            if (
                dirError === "escape" ||
                dirError === "invalid_path" ||
                dirError === "denied" ||
                dirError === "io"
            ) {
                log.warn(`load-save: stat ${saveDir} 失败（${dirError}）⇒ 跳过存档文件前置检查`);
                return true;
            }
            // 目录不存在（not_found / not_dir）⇒ 这个路径下不可能有 `SAVE<NN>.DAT` ⇒ 响亮收口。
            const file = displayPath(appdata, rel);
            const reason =
                `load-save: save file missing: ${file} (slot ${slot};` +
                ` 存档目录 ${saveDir} 不存在)`;
            log.error(reason);
            ctx.finish(5, reason);
            return false;
        }
        const file = displayPath(appdata, rel);
        const reason = `load-save: save file missing: ${file} (slot ${slot})`;
        log.error(reason);
        ctx.finish(5, reason);
        return false;
    }
    return true;
}

const DEFAULT_GATE_TIMEOUT_MS = 30000;

// ★ 载入链的具名门一律 `load:` 前缀（失败文案逐字保留；`chains.js` 的路由已删）。
//   终态的门名 = `load:expect:<脚本名>`（脚本名 = 调用方给的 `expect` 元素，含 `.BIN`）。
const LOAD_EXPECT_PREFIX = "load:expect:";

/**
 * 采样"下一拍"：每 **SAVE 回调体入口**执行一次 ⇒ 一次调用 = 一个 guest 回调（≈ v1 的一个读点）。
 *
 * ★ 为什么不用 `{script: "SAVE.BIN"}`：那是"下一条**指令**"，频率高一个量级 ⇒ `PAGE_STALL_LIMIT`
 *   的 205 拍会在 ~10 ms 烧完（实测页号要 ≈210 ms 才变）⇒ 正常点击被误判成"丢了"。见文件头。
 *
 * @param {RunCtx} ctx
 * @param {string} name 失败文案里的门名（`load:pg:<target>:<phase>:<i>:<last>`，与 v1 逐字同形）。
 * @param {number} timeoutMs
 * @returns {Promise<Tick>}
 */
function sampleSave(ctx, name, timeoutMs) {
    return awaitWhere(
        ctx,
        { script: SCRIPT_SAVE, pos: SAVE_INPUT_CALLBACK_LO },
        name,
        timeoutMs,
        "load-save",
    );
}

/**
 * `loadSave` 的**纯校验**（slot / expect / timeoutMs / saveDir / precheck；**不碰 ctx**）。
 *
 * ★ 为什么单独导出（P1 复审 S3）：v1 的校验在**构造时**（`loadSave(slot, options)` 里），坏 slot /
 *   坏 expect 会**当场** throw ⇒ Run 首个回调就 `run=failed`；P1 把它挪到 `loadSave()` 调用时
 *   （= `await bootToTitle` **之后**）⇒ 坏参数要等 TITLE 才报（启动到不了 TITLE 时只报 boot 门超时，
 *   把"参数非法"与"启动没到"混成同一条）。⇒ 根入口 `load-save.js` 在 **start 的同步首段**
 *   （`ctx.task` 之前）调本函数，把"构造时失败"的时序找回来；库调用方仍由 `loadSave` 自己调一次
 *   （⛔ 不要求调用方记得先校验）。
 * ★ 纯函数：不读 `ctx` / 不读 appdata（缺文件 stat 仍留在 `loadSave` 里、第一个 click 之前）。
 *
 * @param {number} slot 存档槽（读档界面能选到的 0–899；页 = slot/10，页内行 = slot%10）。
 * @param {LoadSaveOptions | null} [options]
 * @returns {{ slot: number, rowY: number, targetPage: number, gateTimeoutMs: number,
 *             saveDir: string, precheck: boolean, expect: string[] }}
 */
export function validateLoadSave(slot, options = {}) {
    const opts = options !== null && options !== undefined ? options : {};
    if (!Number.isInteger(slot) || slot < 0 || slot > MAX_SLOT) {
        throw new Error(`loadSave: slot ${slot} 越界（0–${MAX_SLOT}；读档界面 = 90 页 × 10 行）`);
    }
    const rowY = ROW_Y[slot % SAVE_PAGE_ROWS];
    if (rowY === undefined) {
        throw new Error(`loadSave: slot ${slot} 的页内行 ${slot % SAVE_PAGE_ROWS} 没有实测 y（0–9；见 SAVE.md）`);
    }
    const targetPage = Math.floor(slot / SAVE_PAGE_ROWS);
    const gateTimeoutMs =
        opts.timeoutMs !== undefined ? opts.timeoutMs : DEFAULT_GATE_TIMEOUT_MS;
    if (!Number.isInteger(gateTimeoutMs) || gateTimeoutMs <= 0) {
        throw new Error(`loadSave: options.timeoutMs 必须是正整数（收到 ${String(gateTimeoutMs)}）`);
    }
    const saveDir = normalizeSaveDir(opts.saveDir);
    const precheck = opts.precheck !== undefined ? opts.precheck : true;
    if (typeof precheck !== "boolean") {
        throw new Error(`loadSave: options.precheck 必须是 boolean（收到 ${typeof precheck}）`);
    }
    // ★ 终态由**调用方**给（⛔ 本文件不内置场景清单）：ADV / 战斗 / 地图的脚本名都不一样，
    //   只有使用方知道这次载入"应该落在哪"。缺了就当场 throw（⛔ 不猜一个默认值）。
    const expect = opts.expect;
    if (!Array.isArray(expect) || expect.length === 0) {
        throw new Error(
            "loadSave: options.expect 必填（string[]，非空）—— 载入完成后落在哪个脚本由调用方按预期给：" +
                "ADV = SC*/SG*/SP*（如 SC0000.BIN）、战斗 = BTL.BIN、地图 = FIELD.BIN；" +
                "照宿主读数拼写（带 .BIN）",
        );
    }
    for (let i = 0; i < expect.length; i += 1) {
        if (typeof expect[i] !== "string" || expect[i].length === 0) {
            throw new Error(
                `loadSave: options.expect[${i}] 必须是非空字符串（脚本名，照宿主读数拼写、带 .BIN）`,
            );
        }
    }
    // ★ 校验之后收窄成 `string[]`：调用方（`loadSave` 的闭包）读的是这份局部值 —— TypeScript
    //   不会把外层 `expect` 的收窄带进嵌套函数。
    return { slot, rowY, targetPage, gateTimeoutMs, saveDir, precheck, expect };
}

/**
 * 构造并执行一次 `loadSave`（**async**；进度全在本次调用的局部变量里）。
 *
 * @param {RunCtx} ctx RunCtx（`ctx.once` / `ctx.read` / `ctx.appdata.stat` / `ctx.finish` 都是 Run 侧入口）。
 * @param {number} slot 存档槽（读档界面能选到的 0–899；页 = slot/10，页内行 = slot%10）。
 * @param {LoadSaveOptions | null} options 见 `LoadSaveOptions`。
 * @param {Tick} titleTick **TITLE 命中那一拍的 Tick**：LoadData 那一下点击必须用它（Tick 的写面只在
 *   产生它的那次入口内有效；`ctx.input` 在 task continuation 里是 TypeError）。
 * @returns {Promise<Tick>} 载入终态命中那一拍的 Tick（场景接续用它做写）。
 */
export async function loadSave(ctx, slot, options = {}, titleTick) {
    // ★ 校验集中在 `validateLoadSave`（根入口在 `start` 的同步首段还会先调一次 —— 见其文档）。
    const v = validateLoadSave(slot, options);
    const rowY = v.rowY;
    const targetPage = v.targetPage;
    const gateTimeoutMs = v.gateTimeoutMs;
    const saveDir = v.saveDir;
    const precheck = v.precheck;
    /** @type {string[]} */
    const expected = v.expect;
    if (titleTick === undefined || titleTick === null) {
        throw new Error("loadSave: 需要 TITLE 命中那一拍的 Tick（先 await bootToTitle(ctx)）");
    }

    // ★ **前置检查在第一个 click 之前**（2026-11-25）：缺 `SAVE<NN>.DAT` ⇒ finish(5) + 结束，
    //   ⛔ 不发任何 click（否则已经进了 SAVE 屏，失败原因会和"点行不进确认框"混在一起）。
    //   跳过条件见 `precheckSaveFile`（无 appdata / slot>99 / stat 无法证明 / 逃生口 `precheck:false`）。
    if (!precheck) {
        log.info("load-save: precheck=false ⇒ 跳过存档文件前置检查（宿主声明存档不在 appdata）");
    } else if (!precheckSaveFile(ctx, slot, saveDir)) {
        // Run 已 finished（exit 5）；抛一个标记让 task 结束（`runTerminal` ⇒ 不会覆盖读数）。
        throw new Error("load-save: save file missing（已 finish(5)）");
    }

    // ★ **前提：已经在 TITLE**（文件头）。直接点 LoadData 入口，然后等 SAVE 门。
    //   点不进去（不在 TITLE / 点被吞）⇒ SAVE 门超时 = 响亮失败。
    log.info(`load-save: start (slot ${slot}) ⇒ click LoadData (${LOAD_MENU_X},${LOAD_MENU_Y})`);
    titleTick.input.click(LOAD_MENU_X, LOAD_MENU_Y);
    let tick = await awaitPosition(
        ctx,
        SCRIPT_SAVE,
        SAVE_INPUT_CALLBACK_LO,
        SAVE_INPUT_CALLBACK_HI,
        "load:save",
        gateTimeoutMs,
        "load-save",
    );
    log.info("load-save: SAVE hit");
    // ★ 页号**可读**（`readSavePage`）：先读当前页，再决定要不要翻。
    let seen = readSavePage(ctx);
    log.info(`load-save: page ${seen} (target ${targetPage}, slot ${slot})`);

    if (seen !== targetPage) {
        // 第一拍：等一个 SAVE 回调（v1 在 SAVE 命中后先 arm 一个读点，再决定点哪个方向）。
        // ★ 门名带 `:<last>`（v1 逐字：`load:pg:<page>:<phase>:<i>:<last>`；首拍 last = -1）。
        tick = await sampleSave(ctx, `load:pg:${targetPage}:turn:0:-1`, gateTimeoutMs);
        seen = readSavePage(ctx);
        tick = await walkPages(ctx, tick, seen, targetPage, rowY, gateTimeoutMs, "turn", 0, -1, slot);
    }

    // ---- 页内行：move → 一拍 → 点行前再核页号（v1 `rowMove` + `load:row` 的逐行等价物）----
    for (;;) {
        tick.input.move(SLOT_X, rowY);
        tick = await sampleSave(ctx, "load:row", gateTimeoutMs);
        const nowPage = readSavePage(ctx);
        if (nowPage === targetPage) break;
        // ★ 点行**之前**再核一次页号（2026-11-25）：箭头点击的落地要比 2 个 guest 回调，
        //   旧预算（60 读点）会让页链提前发一次"重试点击"；那一下在页号已经变了之后
        //   才落地 ⇒ 把页**又**翻一格（0 → 89），随后按页内行 y 点下去就落到别的槽上
        //   （空槽 ⇒ 没有确认框 ⇒ `load:sbunki` 30 s 超时）。⇒ 这里发现页不对就回页链，
        //   ⛔ 不点行、⛔ 不猜"差一格"。
        log.warn(
            `load-save: page moved to ${nowPage} before the row click (target ${targetPage}) ⇒ 回页链`,
        );
        tick = await walkPages(ctx, tick, nowPage, targetPage, rowY, gateTimeoutMs, "turn", 0, nowPage, slot);
    }
    log.info("load-save: row hovered");
    tick.input.click(SLOT_X, rowY);
    tick = await awaitPosition(
        ctx,
        SCRIPT_SBUNKI,
        SBUNKI_CONFIRM_CLICK_LO,
        SBUNKI_CONFIRM_CLICK_HI,
        "load:sbunki",
        gateTimeoutMs,
        "load-save",
    );
    log.info("load-save: SBUNKI hit (read-segment passed)");
    tick.input.click(CONFIRM_X, CONFIRM_Y);

    // ★ 终态 = expect 里任一脚本 **或** 第二道确认（同一张对话框的回环）。
    //   v1 用 armed 集合的"完整替换"表达；v2 用 `ctx.waitAny` 的多臂等待（`waitFirst` 薄封装）——
    //   ⛔ 不用 `Promise.race([once])`：输的那条 once 不能撤，会一直算 Run 存活直到 timeout ⇒ fail。
    //   ★ 超时（`gateTimeoutMs`）是**真的 wall-clock 失败截止**（P1 复审 S2）：v1 在这里 30 s 后
    //     `finish(5, "load-save: gate timeout (load:expect:<name>)")`；v2 由 `waitFirst` 的 catch
    //     复现同一文案（`finish(5)` 后重新 throw，task 结束；`waitAny` 的 pending 已撤干净）。
    const first = await waitFirst(ctx, expectTerminals(true), gateTimeoutMs, "load-save");
    if (first.tag === "load:confirm2") {
        log.info("load-save: SBUNKI confirm read-segment ⇒ second confirm");
        first.tick.input.click(CONFIRM2_X, CONFIRM2_Y);
        // ★ 第二次之后**只剩 expect**：SBUNKI 的读段会在离开对话框前再跑一遍，再挂 confirm2 臂
        //   会让它命中自己（v1 的 `terminalAfterConfirm` 也是只剩 expect）。
        const after = await waitFirst(ctx, expectTerminals(false), gateTimeoutMs, "load-save");
        return reached(after, slot);
    }
    return reached(first, slot);

    /**
     * `expect` 的终态臂（`{script}` = 该脚本下一条指令）；`withConfirm2` 时再加第二道确认臂。
     *
     * @param {boolean} withConfirm2
     * @returns {readonly LoadArm[]}
     */
    function expectTerminals(withConfirm2) {
        /** @type {LoadArm[]} */
        const arms = expected.map((script) => ({
            where: { script },
            tag: LOAD_EXPECT_PREFIX + script,
        }));
        if (withConfirm2) {
            arms.push({
                where: {
                    script: SCRIPT_SBUNKI,
                    lo: SBUNKI_CONFIRM_READ_SEG_LO,
                    hi: SBUNKI_CONFIRM_READ_SEG_HI,
                },
                tag: "load:confirm2",
            });
        }
        return arms;
    }
}

/**
 * 终态：先 log 再返回那一拍的 Tick（⛔ 没有 `onLoaded` 了 —— 场景就是调用方的顺序 `await`）。
 *
 * @param {LoadHit} hit
 * @param {number} slot
 * @returns {Tick}
 */
function reached(hit, slot) {
    if (!hit.tag.startsWith(LOAD_EXPECT_PREFIX)) {
        // 走到这里说明多臂等待把"第二道确认"当成了终态 ⇒ 链有 bug，⛔ 不要静默当成落点。
        throw new Error(`loadSave: 终态臂不合法（tag=${hit.tag}）`);
    }
    const script = hit.tag.slice(LOAD_EXPECT_PREFIX.length);
    log.info(`load-save: reached ${script} (slot ${slot})`);
    return hit.tick;
}

/**
 * 翻页状态机（v1 `armPage` / `pageStep` / `onPageState` 的 async 等价物；进度全在局部变量里）。
 *
 * 阶段：
 *   turn<i>  点过箭头之后**等页号真的动**：动了 ⇒ 按新位置再走一步；没动 ⇒ 等；
 *            等满 `PAGE_STALL_LIMIT` 拍 ⇒ 再点一次（`turnr`，点击被吞的第二次机会）
 *   turnr<i> 同上，但再等满一次就 `finish(5)` 响亮失败（⛔ 不无限重试、⛔ 不假设"点一下就到"）
 *
 * @param {RunCtx} ctx
 * @param {Tick} tick 进入状态机时的那一拍（第一次点击用它）。
 * @param {number} seen 这一拍读到的页号。
 * @param {number} targetPage 目标页。
 * @param {number} rowY 本槽的页内行 y（只用于日志文案）。
 * @param {number} timeoutMs 每次采样的失败截止。
 * @param {"turn" | "turnr"} phase 当前阶段。
 * @param {number} i 当前阶段已经等过的拍数。
 * @param {number} last 上一次决定"没动"时的页号（首拍 = -1 ⇒ 任何读数都算"动了"）。
 * @param {number} slot 槽号（日志文案）。
 * @returns {Promise<Tick>} `load:page-ready` 命中那一拍的 Tick。
 */
async function walkPages(ctx, tick, seen, targetPage, rowY, timeoutMs, phase, i, last, slot) {
    for (;;) {
        if (seen === targetPage) {
            log.info(
                `load-save: page ${targetPage} reached (slot ${slot} ⇒ row ${slot % SAVE_PAGE_ROWS} y=${rowY})`,
            );
            // ★ 翻完页**不能马上点行**：页号在点箭头那一刻就变了，而列表要**重读这一页的 10 份存档头**
            //   —— 刷新期间点下去会被吞（实测：page 0 到了、`click` 之后 `load:sbunki` 30 s 超时）。
            //   所以再等一道"SAVE 的回调体又活了"的位置门（列表刷完、在等输入），然后才 move/click。
            return awaitPosition(
                ctx,
                SCRIPT_SAVE,
                SAVE_INPUT_CALLBACK_LO,
                SAVE_INPUT_CALLBACK_HI,
                "load:page-ready",
                timeoutMs,
                "load-save",
            );
        }
        // 位置**动了**（或第一次进这个阶段：last = -1）⇒ 立刻按新位置再走一步。
        if (seen !== last) {
            const forward = (targetPage - seen + 2 * SAVE_PAGES) % SAVE_PAGES;
            const backward = (seen - targetPage + 2 * SAVE_PAGES) % SAVE_PAGES;
            tick.input.click(forward <= backward ? PAGE_NEXT_X : PAGE_PREV_X, PAGE_ARROW_Y);
            last = seen;
            i = 0;
            tick = await sampleSave(ctx, `load:pg:${targetPage}:${phase}:${i}:${last}`, timeoutMs);
            seen = readSavePage(ctx);
            continue;
        }
        if (i < PAGE_STALL_LIMIT) {
            i += 1;
            tick = await sampleSave(ctx, `load:pg:${targetPage}:${phase}:${i}:${last}`, timeoutMs);
            seen = readSavePage(ctx);
            continue;
        }
        // 等满了还没动：第一次 ⇒ 重试一次（点击可能被吞）；第二次 ⇒ 响亮失败。
        if (phase === "turn") {
            phase = PAGE_RETRY_PHASE;
            const forward = (targetPage - seen + 2 * SAVE_PAGES) % SAVE_PAGES;
            const backward = (seen - targetPage + 2 * SAVE_PAGES) % SAVE_PAGES;
            tick.input.click(forward <= backward ? PAGE_NEXT_X : PAGE_PREV_X, PAGE_ARROW_Y);
            last = seen;
            i = 0;
            tick = await sampleSave(ctx, `load:pg:${targetPage}:${phase}:${i}:${last}`, timeoutMs);
            seen = readSavePage(ctx);
            continue;
        }
        ctx.finish(5, `load-save: page not reached (target ${targetPage}, still ${seen})`);
        throw new Error("load-save: page not reached");
    }
}
