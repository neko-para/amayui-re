// scripts/plugins/lib/save.js —— `loadSave(slot)`：**纯 JS 的 callback 链构造器**（C2c）
//
// 合同：`.tmp/plugin-runtime/c2c-decisions.md`（D-C2C-1…6）/ `scripts/plugins/plugin.d.ts`
//       （`RunCtx` / `Trigger` / `FiredEvent` / `TimeoutEvent`）。
//
// ★ **前提：调用方保证已经在 TITLE**（2026-11-21 起，TITLE 门不再在本文件里）：
//   `start(ctx)` 的第一件事就是 `click(1070,480)`（TITLE 的 LoadData 入口），然后 arm SAVE 门。
//   从**冷启动**跑 ⇒ 用 `./boot-to-title.js` 的 `bootToTitle({ onDone: (ctx) => load.start(ctx) })`
//   盖在前面（三段组合的样板 = `./scenario-save07-battle.js`）。
//   ⚠ 不在 TITLE ⇒ 这一下点不进 LoadData ⇒ SAVE 门超时（`load-save: gate timeout (load:save)`）。
//     那是**响亮失败**、也是本文件想要的形态：⛔ 不自己再等一轮 TITLE（那会把"启动没到"与
//     "TITLE 上没点进去"混成同一条超时）。
//
// 链（slot 5 实测；**终态 = 调用方给的 `expect`**，⛔ 本文件不内置场景清单）：
//   click(1070,480) → SAVE [0x1104,0x11C4)
//   → move(300,rowY) → 读点 → click(300,rowY) → SBUNKI [0x0B18,0x0C58)
//   → click(640,323) → [expect 里任一脚本 | SBUNKI 0x0AF8]：
//        expect 命中 ⇒ done（给了 `onLoaded` 就交给它续）
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
//     · 每点一次都**用门等页号真的变**：arm `atReadPoint`，回调里 `readSavePage`；位置动了 ⇒ 按新位置再走一步；
//       没动满 `PAGE_STALL_LIMIT` 个读点 ⇒ 重试**一次**；再等满一次还没动 ⇒ `ctx.finish(5, …)` 响亮失败
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
//   ⇒ 修法（本文件）：预算改 **1200**（≈2.6× 实测上界 461 读点）⇒ 正常时 `turn` 自己就看到页号变、
//   **不产生**重试点击；只有点击真丢了才重试。另在点行**之前**再核一次页号（不符就回页链、⛔ 不点行）。
//   ★ **真游戏 A/B（同一份 `saves/`、同一个 exe）**：把第一下箭头点击**故意点空**（= 模拟"第一下被吞"）⇒
//     旧 60：`run=finished exit=5 reason=load-save: page not reached (target 0, still 1)`（calls=126）；
//     新 1200：`page 0 reached` → `reached REIGN.BIN`（calls=723）⇒ 重试窗口是这条链唯一的救命面。
//   ⛔ 没有放宽任何门（判据仍是"页号真的等于目标页"）；⛔ 没改 `saves/`；⛔ 没有用 `--input-plan`。
//
// ★ 2026-11-21（A 阶段）—— **两个接口，各管一件事**：
//   · `options.expect: string[]`（**必填**）：载入完成后落在哪个脚本，由**调用方按预期给** ——
//     ADV = `SC*` / `SG*` / `SP*`（如 `SC0000.BIN`）、战斗 = `BTL.BIN`、地图 = `FIELD.BIN`（可能还有别的名字）。
//     照宿主读数拼写、**带 `.BIN`**。终态 = `expect.map((s) => afterScript(s, { name: "load:expect:" + s }))`。
//     缺 / 空数组 / 元素不是非空字符串 ⇒ **构造时 throw**（消息里给出三类别名，⛔ 不猜）。
//   · `options.onLoaded(ctx)`：到终态**先 log 再回调**；它的返回值就是本次 `onTrigger` 的返回值
//     （`Trigger | Trigger[] | null`）。⛔ 没给 `onLoaded` 时行为一字不变（`return []`）；
//     它抛错就让它穿到宿主（⛔ 不 try/catch、⛔ 不代它 fail）。
//   · ★「载入 → 场景」的**组合**不在本文件：那是使用方拿 `./chains.js` 的 `route(isFirst, first, then)`
//     自己拼（样板 `./scenario-save07-battle.js`）。本文件**不认识场景**。
//
// ★ 本文件有一条 **default 导出**，但它**只是参考入口**：方便
//   `--plugin <本文件> --plugin-param slot=5 --plugin-param expect=FIELD,SC0000` 直接把本文件当插件跑
//   （headless e2e / 手工 run；`expect` 不给时用默认值**并打一行 info**）。
// 库消费者请用具名 `export function loadSave(slot, { expect, … })`；⛔ 不要依赖 default 的形状。
//   ★ 本文件只 import **同目录的兄弟文件**（`./chains.js` / `./script-positions.js` /
//   `./boot-to-title.js`）：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`）⇒ 以本文件为入口时都解析得到；
//   反过来，`tests/plugin_fixtures/*` **不能** import 本文件 ⇒ 自带的 default 仍必要
//   （pure CTest 与 e2e 直接拿本文件当入口，不复制文件）。
//
// ★ 失败面（全部响亮，⛔ 不静默）：
//   ① slot 非整数 / 不在 0–`MAX_SLOT`（899 = 90 页 × 10 行，界面能选到的范围）⇒ **构造时** throw（参考入口在 `start` 里构造 ⇒ Run fail + 点名）；⚠ 槽 ≥100 的文件名字形**未取证**；
//   ①' `expect` 缺 / 空 / 元素不是非空字符串 ⇒ **构造时** throw（消息给出 ADV / 战斗 / 地图三类别名）；
//   ② 未知 `ev.name` ⇒ `ctx.fail(ev.name, …)` + `[]`；
//   ③ 门超时（`timeoutMs`）⇒ `ctx.finish(5, "load-save: gate timeout …")` + `[]`。
//      ⛔ 不用 `ctx.quit`（那会把归因改成 `plugin: quit`）/ ⛔ 不用 `ctx.run`（C5）。
// ★ 本对象**无状态**（进度只在 `ev.name`）：宿主每回调新建实例（`plugin-runtime.md` §4.6），
//   实现必须支持"新实例接旧链"；⛔ 不把进度塞进闭包 / `ctx.state`。

// ★ 位置常量与"启动到 TITLE"都是**兄弟模块**（同目录 ⇒ 以本文件为入口时 import 解析得到）。
//   本文件从"已经在 TITLE"开始 ⇒ TITLE 门在 `./boot-to-title.js` 里，不在本文件。
import { bootToTitle } from "./boot-to-title.js";
import { route } from "./chains.js";
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
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */
/**
 * `loadSave` 的选项。`expect` 在**运行期**必填（缺 / 空 / 元素不是非空字符串 ⇒ 构造时 throw），
 * 这里标 `?` 只是为了让"省略 options / 省略 expect"能通过类型检查后由那段运行期校验响亮拒绝。
 *
 * @typedef {object} LoadSaveOptions
 * @property {string[]} [expect] 载入完成后**可以算落点**的脚本名表（必填，见上）。
 * @property {number} [timeoutMs] 每道门的超时；缺省 30000 ms。
 * @property {(ctx: RunCtx) => Trigger | Trigger[] | null} [onLoaded] 到终态之后要做的事。
 */
/**
 * 一条链：形状 = `PluginModule` 的 `start` / `onTrigger`（与 `./chains.js` 的 `Chain` 同形）。
 *
 * @typedef {object} Chain
 * @property {(ctx: RunCtx) => Trigger | Trigger[] | null} start
 * @property {(ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] | null} onTrigger
 */

// ★ 脚本身份 = **marker 读数**（`PluginReadings::sample` ⇒ `MarkerSampler::resolveName`），实际是
//   文件名 `TITLE.BIN` / `SAVE.BIN` / …（实测：`--plugin` 探针在 headless boot 读到
//   `SYSTEM4.BIN` / `LOADCONFIG.BIN` / `TITLE.BIN`）。C1 的 `afterScript` / `afterPosition` 是
//   **精确 `==`**（`src/runtime/trigger_runner.cpp`：`now.script == trigger.script`），⛔ 不做
//   `await script` 那层的"两侧剥 `.BIN` + ASCII 大小写不敏感"（`src/runtime/recipe.cpp`）。
//   ⇒ 链里的名字必须带 `.BIN`，否则门永远不命中（纯假读数用例也要照这个拼写）。
//   ⚠ 这是 API 文档口径的已知缺口（`plugin.d.ts` / `plugin-runtime.md` 的样本写的是裸名
//   `TITLE`）：归 docs 轮裁决；本文件按**宿主实际读数**写，⛔ 不假设将来会归一化。
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
// 点一次箭头后等"页号真的变了"的读点上限。
//
// ★ 2026-11-25 改 60 → 1200（**回归修复**；读数见文件头 §2026-11-25）：
//   实测一次箭头点击从"提交"到"页号变"= **76–77 个读点 / ≈210 ms**（5/5 次无干扰的探针 run：
//   76,77,76,76,77；页号变的那一拍落在 `SAVE.BIN` 的翻页处理段 `pos=0x6EF4` / `0x6F44`），
//   读点密度 ≈ **366 个/s** ⇒ **60 个读点 ≈ 164 ms ≈ 1.5 帧**，而点击的抬起要等按下之后的
//   **两个完整 guest 回调**（`PointerCore::kCallbackFramesBeforeRelease`，闸门②′）⇒ 旧值**比点击自身还短**。
//   真 run 里的落地延迟还**在抖**：12 次 `save.js` run 的页链回调数 = 85…461（≈0.2–1.3 s，
//   抖动来自"2 个回调"的帧长本身）⇒ 上界按 **461 读点**取。
//   ⛔ 旧注释写的"一个读点 ≈ 一拍输入轮询 ⇒ 60 拍 ≈ 1 s"是错的（实测 60 拍 ≈ 0.16 s）——
//   这条错误假设就是 60 的来路，也是 11-24「3 次 2 次 `page not reached`」的来路：
//   · 每一次**正常**点击都会先撞满 60、再靠 `turnr` 那一拍才接住（所以"有时能过"）；
//   · 第一下真被吞时，`turnr` 的重试点击自己又要 76 个读点才落地，而重试预算只有 60
//     ⇒ 重试**结构上救不回来**，必红（真游戏里复现过：见文件头）。
//   新值 = **1200**（≈2.6× 实测上界）：正常时 `turn` 阶段自己就看到页号变（**不产生**重试点击，
//   也就不会有多余的第 2 次翻页）；只有点击真丢了才走重试（最坏等 2×1200 读点后响亮失败）。
const PAGE_STALL_LIMIT = 1200;
// 点击被吞时的第二次机会（同样是 PAGE_STALL_LIMIT 个读点）。
const PAGE_RETRY_PHASE = "turnr";

// 行 y 的**实测表**（⛔ 不用 91 + 59.5*n 之类的公式猜；0–7 与 9 都有读数，8 与截图逐行吻合）。
/** @type {Readonly<Record<number, number | undefined>>} */
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447, 7: 507, 8: 567, 9: 626 };

// 槽号上限：读档界面 = 90 页 × 10 行（页签条实测到"890"，页号 0..89 环形）⇒ 手选槽 0..899。
// ⚠ 槽 ≥ 100 的**文件名字形**未取证（`saves/` 只到 `SAVE99`；`%2.2d` 遇 3 位数会照写）⇒ 本库只按
//   "界面能选到的槽"放行，⛔ 不声称 SAVE100.DAT 一定能打开。
const MAX_SLOT = 90 * SAVE_PAGE_ROWS - 1;

const DEFAULT_GATE_TIMEOUT_MS = 30000;

// ★ 载入链的具名状态一律 `load:` 前缀；`chains.js` 的 `route` 与场景命名禁区都只看这一个前缀。
//   终态的具名状态 = `load:expect:<脚本名>`（脚本名 = 调用方给的 `expect` 元素，含 `.BIN`）。
const LOAD_EXPECT_PREFIX = "load:expect:";

/**
 * 构造一条 `loadSave(slot, options)` 的 callback 链（纯函数：只读 slot / options，不读 ctx）。
 *
 * @param {number} slot 存档槽（读档界面能选到的 0–899；页 = slot/10，页内行 = slot%10）。
 * @param {LoadSaveOptions | null} [options]
 *   · `expect`（**必填**）：载入完成后**可以算落点**的脚本名表（照宿主读数拼写、带 `.BIN`）——
 *     ADV = `SC*`/`SG*`/`SP*`、战斗 = `BTL.BIN`、地图 = `FIELD.BIN` …；由**调用方按预期给**。
 *   · `timeoutMs`：每道门的超时；缺省 30000 ms。
 *   · `onLoaded`：载入**到终态**之后要做的事（终态先 `ctx.log` 再回调）；它的返回值就是本次
 *     `onTrigger` 的返回值。⚠ 抛错**不**在这里捕获 —— 交给宿主按既有规则记 fail。
 * @returns {Chain}
 */
export function loadSave(slot, options = {}) {
    if (!Number.isInteger(slot) || slot < 0 || slot > MAX_SLOT) {
        throw new Error(`loadSave: slot ${slot} 越界（0–${MAX_SLOT}；读档界面 = 90 页 × 10 行）`);
    }
    const rowY = ROW_Y[slot % SAVE_PAGE_ROWS];
    if (rowY === undefined) {
        throw new Error(`loadSave: slot ${slot} 的页内行 ${slot % SAVE_PAGE_ROWS} 没有实测 y（0–9；见 SAVE.md）`);
    }
    const targetPage = Math.floor(slot / SAVE_PAGE_ROWS);
    const gateTimeoutMs =
        options !== null && options.timeoutMs !== undefined ? options.timeoutMs : DEFAULT_GATE_TIMEOUT_MS;
    const onLoaded = options !== null ? options.onLoaded : undefined;
    if (onLoaded !== undefined && typeof onLoaded !== "function") {
        throw new Error(`onLoaded must be a function (got ${typeof onLoaded})`);
    }
    // ★ 终态由**调用方**给（⛔ 本文件不内置场景清单）：ADV / 战斗 / 地图的脚本名都不一样，
    //   只有使用方知道这次载入"应该落在哪"。缺了就当场 throw（⛔ 不猜一个默认值）。
    const expect = options !== null ? options.expect : undefined;
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

    // 终态 = 调用方给的**期望落点表**（⛔ 本文件不内置场景清单）。
    // ★ 具名门：一个 armed 集合内 name 唯一；每个回调返回**完整的新集合**（⛔ 不是增量）。
    //   第二道确认之后也用它（那里同样只认调用方的 `expect`；点 690,323 那一步在 onTrigger 里）。
    /** @type {(ctx: RunCtx) => Trigger[]} */
    const expectTerminals = (ctx) =>
        expect.map((script) =>
            ctx.trigger.afterScript(script, {
                name: LOAD_EXPECT_PREFIX + script,
                timeoutMs: gateTimeoutMs,
            }),
        );

    // 点完 640,323 之后的终态集合：`expect` 里任一脚本 **或** 第二道确认（同一张对话框的回环）。
    /** @type {(ctx: RunCtx) => Trigger[]} */
    const terminal = (ctx) => {
        const gates = expectTerminals(ctx);
        gates.push(
            ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CONFIRM_READ_SEG_LO, SBUNKI_CONFIRM_READ_SEG_HI, {
                name: "load:confirm2",
                timeoutMs: gateTimeoutMs,
            }),
        );
        return gates;
    };

    // 第二道确认之后的终态：只剩 `expect` 里的脚本。
    /** @type {(ctx: RunCtx) => Trigger[]} */
    const terminalAfterConfirm = (ctx) => expectTerminals(ctx);

    /** @type {(ctx: RunCtx, script: string) => Trigger | Trigger[] | null} */
    const reached = (ctx, script) => {
        ctx.log("info", `load-save: reached ${script} (slot ${slot})`);
        // ★ 终态**先 log 再回调**：`onLoaded` 的返回值 = 本次 onTrigger 的返回值。
        //   ⛔ 没给 `onLoaded` ⇒ 与 C2c 一字不变（`return []` = done）。⛔ 不 try/catch：
        //   它抛错要按宿主的既有规则记 fail（有 file:line 归因），吞掉就变成"静默 done"。
        if (onLoaded === undefined) return [];
        return onLoaded(ctx);
    };

    // ---- 页：读档界面每页 10 行；槽 010 在**第 2 页**（页号 = 可读的 guest int global）------------
    //
    // ★ 进度**只在 `ev.name`**（宿主每回调新建实例）：页链的名字 =
    //   `load:pg:<目标页>:<阶段>:<计数>:<上次看到的位置>` —— 全部从名字解析 ⇒ 不需要闭包 / `ctx.state`。
    const PAGE_PREFIX = "load:pg:";

    /** @type {(ctx: RunCtx, page: number, phase: string, i: number, last: number) => Trigger} */
    const armPage = (ctx, page, phase, i, last) =>
        ctx.trigger.atReadPoint("any", 1, {
            name: `${PAGE_PREFIX}${page}:${phase}:${i}:${last}`,
            timeoutMs: gateTimeoutMs,
        });

    // 页内行：move → 一个读点 → click（与 C2c 一字不变的两段形状）。
    /** @type {(ctx: RunCtx) => Trigger} */
    const rowMove = (ctx) => {
        ctx.input.move(SLOT_X, rowY);
        return ctx.trigger.atReadPoint("any", 1, { name: "load:row", timeoutMs: gateTimeoutMs });
    };

    // 往目标页**走一步**：比当前页大 ⇒ 右箭头（下一页），小 ⇒ 左箭头（上一页）；两端环形
    //   （0 的上一页 = 89）。方向按"更短的那一边"选，⛔ 不假设"只能一步一步往前"。
    /** @type {(ctx: RunCtx, ctxPage: number, seen: number, phase: string) => Trigger} */
    const pageStep = (ctx, ctxPage, seen, phase) => {
        const forward = (ctxPage - seen + 2 * SAVE_PAGES) % SAVE_PAGES;
        const backward = (seen - ctxPage + 2 * SAVE_PAGES) % SAVE_PAGES;
        ctx.input.click(forward <= backward ? PAGE_NEXT_X : PAGE_PREV_X, PAGE_ARROW_Y);
        return armPage(ctx, ctxPage, phase, 0, seen);
    };

    // 页链的两个阶段（见文件头）：
    //   turn<i>  点过箭头之后**等页号真的动**：动了 ⇒ 按新位置再走一步；没动 ⇒ 等；
    //            等满 PAGE_STALL_LIMIT 个读点 ⇒ 再点一次（`turnr`，点击被吞的第二次机会）
    //   turnr<i> 同上，但再等满一次就 `finish(5)` 响亮失败（⛔ 不无限重试、⛔ 不假设"点一下就到"）
    /** @type {(ctx: RunCtx, name: string) => Trigger | Trigger[] | null} */
    const onPageState = (ctx, name) => {
        const parts = name.split(":"); // ["load","pg",<目标页>,<阶段>,<计数>,<上次位置>]
        const page = Number(parts[2]);
        const phase = parts[3];
        const i = Number(parts[4]);
        const last = Number(parts[5]);
        if (!Number.isInteger(page) || !Number.isInteger(i) || !Number.isInteger(last)) {
            ctx.fail(name, `load-save: 页状态名不合法 (${name})`);
            return [];
        }
        if (phase !== "turn" && phase !== PAGE_RETRY_PHASE) {
            ctx.fail(name, `load-save: 未知页阶段 (${name})`);
            return [];
        }
        const seen = readSavePage(ctx);
        if (seen === page) {
            ctx.log("info", `load-save: page ${page} reached (slot ${slot} ⇒ row ${slot % SAVE_PAGE_ROWS})`);
            // ★ 翻完页**不能马上点行**：页号在点箭头那一刻就变了，而列表要**重读这一页的 10 份存档头**
            //   —— 刷新期间点下去会被吞（实测：page 0 到了、`click` 之后 `load:sbunki` 30 s 超时）。
            //   所以再等一道"SAVE 的回调体又活了"的位置门（列表刷完、在等输入），然后才 move/click。
            return ctx.trigger.afterPosition(SCRIPT_SAVE, SAVE_INPUT_CALLBACK_LO, SAVE_INPUT_CALLBACK_HI, {
                name: "load:page-ready",
                timeoutMs: gateTimeoutMs,
            });
        }
        // 位置**动了**（或第一次进这个阶段：last = -1）⇒ 立刻按新位置再走一步。
        if (seen !== last) return pageStep(ctx, page, seen, phase);
        if (i < PAGE_STALL_LIMIT) return armPage(ctx, page, phase, i + 1, last);
        // 等满了还没动：第一次 ⇒ 重试一次（点击可能被吞）；第二次 ⇒ 响亮失败。
        if (phase === "turn") return pageStep(ctx, page, seen, PAGE_RETRY_PHASE);
        ctx.finish(5, `load-save: page not reached (target ${page}, still ${seen})`);
        return [];
    };

    return {
        start(ctx) {
            // ★ **前提：已经在 TITLE**（文件头）。这里不再等 TITLE —— 直接点 LoadData 入口，
            //   然后 arm SAVE 门。点不进去（不在 TITLE / 点被吞）⇒ SAVE 门超时 = 响亮失败。
            ctx.log("info", `load-save: start (slot ${slot}) ⇒ click LoadData (${LOAD_MENU_X},${LOAD_MENU_Y})`);
            ctx.input.click(LOAD_MENU_X, LOAD_MENU_Y);
            return ctx.trigger.afterPosition(SCRIPT_SAVE, SAVE_INPUT_CALLBACK_LO, SAVE_INPUT_CALLBACK_HI, {
                name: "load:save",
                timeoutMs: gateTimeoutMs,
            });
        },

        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") {
                // D-C2C-2：门超时 = finish(5)（⛔ 不用 quit：归因会变成 `plugin: quit`）。
                ctx.finish(5, `load-save: gate timeout (${ev.name})`);
                return [];
            }
            switch (ev.name) {
                case "load:save": {
                    ctx.log("info", "load-save: SAVE hit");
                    // ★ 页号**可读**（`readSavePage`）：先读当前页，再决定要不要翻。
                    const seen = readSavePage(ctx);
                    ctx.log("info", `load-save: page ${seen} (target ${targetPage}, slot ${slot})`);
                    if (seen === targetPage) return rowMove(ctx);
                    // 不在目标页 ⇒ 走箭头（±1 页、环形）；`last = -1` ⇒ 第一拍就点（见 `onPageState`）。
                    return armPage(ctx, targetPage, "turn", 0, -1);
                }
                case "load:page-ready":
                    // 列表刷完了（SAVE 的回调体又活了）⇒ 现在才 move/click 那一行。
                    return rowMove(ctx);
                case "load:row": {
                    // ★ 点行**之前**再核一次页号（2026-11-25）：箭头点击的落地要比 2 个 guest 回调，
                    //   旧预算（60 读点）会让页链提前发一次"重试点击"；那一下在页号已经变了之后
                    //   才落地 ⇒ 把页**又**翻一格（0 → 89），随后按页内行 y 点下去就落到别的槽上
                    //   （空槽 ⇒ 没有确认框 ⇒ `load:sbunki` 30 s 超时）。⇒ 这里发现页不对就回页链，
                    //   ⛔ 不点行、⛔ 不猜"差一格"。
                    const nowPage = readSavePage(ctx);
                    if (nowPage !== targetPage) {
                        ctx.log(
                            "warn",
                            `load-save: page moved to ${nowPage} before the row click (target ${targetPage}) ⇒ 回页链`,
                        );
                        return armPage(ctx, targetPage, "turn", 0, nowPage);
                    }
                    ctx.log("info", "load-save: row hovered");
                    ctx.input.click(SLOT_X, rowY);
                    return ctx.trigger.afterPosition(
                        SCRIPT_SBUNKI,
                        SBUNKI_CONFIRM_CLICK_LO,
                        SBUNKI_CONFIRM_CLICK_HI,
                        {
                            name: "load:sbunki",
                            timeoutMs: gateTimeoutMs,
                        },
                    );
                }
                case "load:sbunki":
                    ctx.log("info", "load-save: SBUNKI hit (read-segment passed)");
                    ctx.input.click(CONFIRM_X, CONFIRM_Y);
                    return terminal(ctx);
                case "load:confirm2":
                    ctx.log("info", "load-save: SBUNKI confirm read-segment ⇒ second confirm");
                    ctx.input.click(CONFIRM2_X, CONFIRM2_Y);
                    return terminalAfterConfirm(ctx);
                default:
                    // ★ 页链（`load:pg:<目标页>:<阶段>:<计数>`）——进度全在名字里。
                    if (ev.name.startsWith(PAGE_PREFIX)) {
                        return onPageState(ctx, ev.name);
                    }
                    // ★ 终态：`load:expect:<脚本名>`（脚本名 = 调用方给的 `expect` 元素）。
                    if (ev.name.startsWith(LOAD_EXPECT_PREFIX)) {
                        return reached(ctx, ev.name.slice(LOAD_EXPECT_PREFIX.length));
                    }
                    ctx.fail(ev.name, `unknown state: ${ev.name}`);
                    return [];
            }
        },
    };
}

// ---------------------------------------------------------------------------
// 参考入口（**只是参考**；库消费者用上面的 `loadSave`）
//
// `--plugin <本文件> --plugin-param slot=5 [--plugin-param expect=FIELD,SC0000]`
// （`ctx.params` 只读）。规则：
//   · `slot`：非整数 / 越界 ⇒ `loadSave` 构造时 throw ⇒ 宿主记 `run=failed`（进程 exit 不受影响）。
//   · `expect`：逗号分隔的**脚本名**（裸名自动补 `.BIN`，带 `.BIN` 也接受）。
//     ★ 不给 ⇒ 退回 `FIELD,SC0000` **并打一行 info**（"用的是默认 expect，可用 --plugin-param expect= 覆盖"；
//      ⛔ 不静默 —— 真实落点由调用方按预期给，这里只是让它能直接当插件跑）。
//
// ★ 参考入口**自带 TITLE 门**（`./boot-to-title.js`）：它可以在**任意启动点**当插件直接跑
//   （`plugin/load-save-e2e` 就是这么用的），所以形状 = `boot:` 前缀归 boot、其余归 load。
//   ⚠ 这与库接口不同：`loadSave(...)` 单独用时**调用方**保证已在 TITLE（文件头）。
// ---------------------------------------------------------------------------
/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {number}
 */
function slotFromParams(params) {
    return Number(params["slot"]);
}

const DEFAULT_EXPECT = ["FIELD.BIN", "SC0000.BIN"];

// null = 参数没给（由调用方决定用默认值 + 打一行说明）；数组 = 参数给了（可能为空 ⇒ 让 loadSave 响亮 throw）。
/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {string[] | null}
 */
function expectFromParams(params) {
    const raw = params["expect"];
    if (raw === undefined || raw === null || String(raw).trim() === "") return null;
    const parts = String(raw).split(",");
    const out = [];
    for (let i = 0; i < parts.length; i += 1) {
        const name = parts[i].trim();
        if (name.length === 0) continue;
        out.push(name.endsWith(".BIN") ? name : name + ".BIN");
    }
    return out;
}

/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {LoadSaveOptions}
 */
function loadOptionsFromParams(params) {
    const expect = expectFromParams(params);
    return { expect: expect === null ? DEFAULT_EXPECT : expect };
}

// ★ 参考入口的链**只构造一次**（`ctx.params` 在一个 Run 内不变；链本身无状态 ⇒ 缓存链对象
//   不影响"进度只在 ev.name"）。⚠ 这**不是**在修预算问题：2026-11-21 实测缓存与"每次重建"
//   在 `plugin/load-save-e2e` 上**同样**会撞 `onTrigger: callback budget exceeded: load:confirm2`
//   （2 ms 那条线；跟踪单 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`，⛔ 不靠放宽预算 / 删日志来"修"）。
//   缓存的收益只是参考入口每拍少造 3 个对象。
/** @type {string | null} */
let entryKey = null;
/** @type {Chain | null} */
let entryChain = null;

/**
 * `boot:` 前缀归启动链（含它的 timeout），其余归载入链 —— 两段名字不相交（`./chains.js` 头）。
 *
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {Chain}
 */
function chainForParams(params) {
    const key = `${String(params["slot"])}|${String(params["expect"])}`;
    if (entryChain !== null && entryKey === key) return entryChain;
    const load = loadSave(slotFromParams(params), loadOptionsFromParams(params));
    const boot = bootToTitle({ onDone: (ctx) => load.start(ctx) });
    entryChain = route((ev) => String(ev.name).startsWith("boot:"), boot, load);
    entryKey = key;
    return entryChain;
}

/** @type {Chain} */
const plugin = {
    start(ctx) {
        if (expectFromParams(ctx.params) === null) {
            ctx.log(
                "info",
                `load-save: expect 未给 ⇒ 用默认 ${DEFAULT_EXPECT.join(",")}` +
                    "（可用 --plugin-param expect=FIELD,SC0000 覆盖）",
            );
        }
        return chainForParams(ctx.params).start(ctx);
    },
    onTrigger(ctx, ev) {
        return chainForParams(ctx.params).onTrigger(ctx, ev);
    },
};

export default plugin;
