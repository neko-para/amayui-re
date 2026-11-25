// scripts/plugins/lib/script-positions.js —— **生成物**（由 tools/script-positions.mjs 生成，⛔ 别手改）
//
// 真源 = `tools/script-positions.json`（**锚表**：基线指令窗口 + 基线行序 ⇒ 补丁后 `pos`）。
// 怎么查：`pnpm tools script-positions explain <NAME>`（锚 → 命中的补丁后指令 → `pos`）。
// 怎么改：改锚表（或改 `data/translations/patch.json` 之后）跑
//         `pnpm tools script-positions gen --write`；`pnpm tools script-positions check` 红 = 忘了 gen。
// 字段语义 / 不变量：`pnpm tools script-positions describe`；说明书：`tools/script-positions.md`。
//
// 口径（下面每条数值都由"锚 + 补丁后视图"现算，⛔ 不要在这个文件里手改）：
//   1. `pos` = **脚本体内绝对字节偏移**，与 `dist/views/data/<NAME>.BIN.txt` 的 `label_%08x` 同坐标系
//      （`pos = 0x3C + Σ(4+argc*8)`；推导与实测见 `docs/03-emulator/input-plan.md` §2.6 / §2.8）。
//   2. 命名：`<SCRIPT>_<用途>_LO` / `_HI`（**半开区间** `[LO,HI)`）或 `<SCRIPT>_<用途>_POS`（单点）。
//      `<SCRIPT>` = 宿主 marker 的**裸名**（`TITLE` / `SBUNKI` / …）—— 这是位置表的命名，**不是** trigger 的
//      `script` 参数：C1 的门是精确 `==`，那边必须写带 `.BIN` 的宿主拼写（见 `save.js` 头）。
//   3. ⛔ **别处不许再裸写这些字面量**。`scripts/plugins/lib/*.js`（除本文件与 `guest-globals.js`）里出现
//      `0x…` 字面量 ⇒ `tools/test/script-map.test.mjs` 红（它**剥注释**后扫，所以头注释里印区间是允许的）。
//      新增一条位置的正确做法：把锚加进 `tools/script-positions.json`，再 `gen --write`（消费方 `import` 名字）。
//
// 收录判据：**有读数支撑**（门命中 / 采样读数 / 清单行号旁证三者之一），且**当前有消费方或作负对照**。
//   · 出处 / 重复定义 / 名字映射 / `<待核>` 全在 `.tmp/script-map/provenance.md`（那只是取证台账，不参与运行）。
//   · 本文件**不收**：`<待核>` 且当前没有任何 lib 消费方的条目 —— `SBUNKI_CLICK_OLD`（0x09F8 旧窗口）、
//     `SC0000_LOAD_STOP`、`SC4000_FIRST/SECOND/TEXT/TEXT2/TEXT3/TEXT4`、`BUNKI_DIALOG`。
//     它们只被 `.plan` 当字面量用（不入 JS），清单与「为什么没收」见 provenance §4 第 1 / 5 条。
//   · 坐标（`1070,480` / `300` / `640,323` / `690,323` / `1220,45` / `640,517` …）**不是** `pos`，
//     ⛔ 不要塞进本文件（它们属于 1280×720 参考系；台账见 provenance §6）。
//   · **SN0000.BIN 的 10 个常量已移出本表**（8/10 不在补丁后指令网格上）—— 重锚之后再回来，见
//     `tools/script-positions.md` 的「未做 / 待核」。
//
// 坐标系的**唯一例外**：`SC0000` / `SC4000` / `BUNKI` 的位置值绑**安装修订版**（`links/raw` 与
// `links/install` 的清单差 `+0x20` / `−0x1C`，见 `docs/03-emulator/scripts/SC0000.md`）⇒ 换安装树会
// **静默不命中**。本文件目前不含这三个脚本（理由见上）。

// ===========================================================================
// TITLE.BIN（doc: `docs/03-emulator/scripts/TITLE.md`）
// ===========================================================================

// 回调体的输入段 = **吃 click 的窗口**（门 15/15；区间上界到 `read-mouse-button` 末尾 `0x514`+3 字）。
export const TITLE_INPUT_CALLBACK_LO = 0x0460;
export const TITLE_INPUT_CALLBACK_HI = 0x0520;

// 主轮询体（`get-input-type` + `sleep 1` + `jmp`）：门会命中但**点击无效** —— "假阳性"的来源。
export const TITLE_POLL_LO = 0x039c;
export const TITLE_POLL_HI = 0x0460;

// 出口段（轮询期**永不执行**）⇒ 负对照门（3/3 `fired=0` + `pending=5` + exit 5）；⛔ 不要当正门。
export const TITLE_EXIT_NEVER_LO = 0x388c;
export const TITLE_EXIT_NEVER_HI = 0x38a4;

// 回调体第一句 `call` 的 `i20c` 块入口（稳态采样 23/40 落在这里；控制层 `pause` 用它）。
export const TITLE_I20C_POS = 0x3844;

// ===========================================================================
// GAMESTART.BIN（doc: `docs/03-emulator/scripts/GAMESTART.md`）
// ===========================================================================

// 回调体的输入读段（回调入口 + `call 0x0680` 整条读输入链）= 吃 click 的窗口（门 7/7 + 配方侧 15/15）。
export const GAMESTART_INPUT_READ_LO = 0x0664;
export const GAMESTART_INPUT_READ_HI = 0x0724;

// 主轮询体：采样最常落在这里（24/47），但它**不是**输入读段。
export const GAMESTART_POLL_LO = 0x05d8;
export const GAMESTART_POLL_HI = 0x0664;

// 回调体第一句 `call 0x50b0` 的 `i20c` 块：**备选**点击门（门 1/1 + 控制层直投 2/2；1 次 run 不够）。
export const GAMESTART_I20C_LO = 0x50b0;
export const GAMESTART_I20C_HI = 0x50b4;

// ===========================================================================
// FELLOW.BIN（doc: `docs/03-emulator/scripts/FELLOW.md`）
// ===========================================================================

// 输入读段（`read-mouse-pos`@0x1684 + `read-mouse-button`@0x1698）★ **推荐门**：窄、点得进去（4/4）。
export const FELLOW_READ_INPUT_LO = 0x1684;
export const FELLOW_READ_INPUT_HI = 0x16a4;

// 整个回调体（入口 → `read-mouse-button` 末尾）：门可用、点击也生效（1/1）。
export const FELLOW_CALLBACK_LO = 0x15a8;
export const FELLOW_CALLBACK_HI = 0x16a4;

// 回调体前半（`call 0x1600` / `eq,jcc` / `call-frame 1c` / `i20c`）：也能吃 click（1/1，反直觉）。
export const FELLOW_CALLBACK_HEAD_LO = 0x15a8;
export const FELLOW_CALLBACK_HEAD_HI = 0x1600;

// 主轮询体：门命中、click 也施加，但**点击不生效** ⇒ 负对照（exit 5）。
export const FELLOW_POLL_LO = 0x1570;
export const FELLOW_POLL_HI = 0x15a8;

// 回调体末尾 `i20c` 单点（稳态采样 16/45；控制层 `pause` 用它）。
export const FELLOW_I20C_POS = 0x15f8;

// ===========================================================================
// SAVE.BIN（doc: `docs/03-emulator/scripts/SAVE.md`）
// ===========================================================================

// 回调体（入口 → `read-mouse-button` 末尾）。★ **保存屏与加载屏共用这一条**（门 3/3：保存 2/2 + 加载 1/1）
// ⇒ 位置谓词区分不了"保存屏 / 加载屏"，那要靠 `(global-int a9cb)`（REQ-01M7J97QY66KMP5SBPCTR0S3TP）。
export const SAVE_INPUT_CALLBACK_LO = 0x1104;
export const SAVE_INPUT_CALLBACK_HI = 0x11c4;

// 主轮询体（`get-input-type` / `sleep 1`@0x10EC / `jmp 0xFF4`）：采样 75/75 落在这里；**没当过门**。
export const SAVE_POLL_LO = 0xff4;
export const SAVE_POLL_HI = 0x1104;

// 回调体第二句 `call` 的被调方块入口（`emu-ctl screenshot` frame=523 · `script=SAVE.BIN pos=0xB39C`）。
// 稳态采样常落在它；**不是门**。
export const SAVE_B39C_BLOCK_POS = 0xb39c;

// ===========================================================================
// SBUNKI.BIN（无 doc；正文散见 `docs/03-emulator/scripts/SC0000.md` 第 2–3 轮 / `plugin-runtime.md` §4.6）
//
// ★ 确认框「要读取吗？」上有**三个相邻但语义不同**的段，别再把它们混成一个"点击窗口"：
//     [0x09A4,0x09F8) 轮询体        —— 对话框已起来、等输入，**点击在这里被吞**
//     [0x0AF8,0x0B18) 读输入段      —— 在**读段里**点也被吞 ⇒ 只适合当"第二道确认"的回环门
//     [0x0B18,0x0C58) 读段之后      —— ★ **吃 click 的窗口**（trace 3/3 被消费）
//   ⚠ 已被取代的"旧窗口" `[0x09F8,0x0B18)`：落点多数在 `0x0AF8`（读段首）⇒ 点击被吞、链停在那里
//     （`REQ-01M7W2X81F1ASB488EJFQGMQSH` 的签名：`gate timeout (load:field-2)`）。
//     它**不在本文件**（`<待核>` 且没有 lib 消费方；3 份 `.plan` 仍以字面量用它 —— provenance §4 第 1 条）。
// ===========================================================================

// 主轮询体（对话框已起来、在等输入）：**点击在这里被吞**。
export const SBUNKI_POLL_LO = 0x09a4;
export const SBUNKI_POLL_HI = 0x09f8;

// 确认框的**读输入段**（`read-mouse-pos`@0x0AF8 / `read-mouse-button`@0x0B0C）：在读段里点**也被吞**。
// `save.js` 拿它当"第二道确认"回环门（`load:confirm2`：落在这里 ⇒ 再点 690,323）。
export const SBUNKI_CONFIRM_READ_SEG_LO = 0x0af8;
export const SBUNKI_CONFIRM_READ_SEG_HI = 0x0b18;

// **读段之后** = 吃 click 的窗口（`save07-battle.plan` 的 trace：3/3 被消费，落点 0xB18/0xB34/0xB70）。
// ★ 与旧窗口 `[0x09F8,0x0B18)` 的关系：旧窗口**跨进**读段 ⇒ 落点常在 0x0AF8 被吞；本条整体在读段之后。
export const SBUNKI_CONFIRM_CLICK_LO = 0x0b18;
export const SBUNKI_CONFIRM_CLICK_HI = 0x0c58;

// 第三次回调里"抬起那一拍"走到的块（`b&1` 清、`b&4` 过 ⇒ 进 0x0F60；`0x1040` 是跳转目标）。
export const SBUNKI_BUTTON_UP_LO = 0x0f60;
export const SBUNKI_BUTTON_UP_HI = 0x1040;

// 确认动作块（"はい"真正生效那一段）：第 3 轮 `load24` 通过（`steps=12 fired=4 pending=0`）。
// ⚠ 第 2 轮 8 s 到期 ≠ 矛盾：那时"输入施加形状"还没修好（`SC0000.md`）。
export const SBUNKI_CONFIRM_ACTION_LO = 0x30ac;
export const SBUNKI_CONFIRM_ACTION_HI = 0x32e8;

// ===========================================================================
// FIELD.BIN（无 doc；读数 = `save07-battle.plan` 头的 `--input-plan-trace`，每 100 ms 采样）
// ===========================================================================

// 主轮询体（地图就绪、在等输入）。门 = 读档完成后"地图在跑"（无 3× 独立读数）。
export const FIELD_MAIN_LOOP_LO = 0x579c;
export const FIELD_MAIN_LOOP_HI = 0x5860;

// ===========================================================================
// REIGN.BIN（无 doc；读数 = `scripts/recipes/fellow-switch.plan` 的 ④ + 该文件头的取证口径）
//
// ★ 两段，都有消费方（`../scenario-fellow-switch.js` 的右键那一步）：
//   回调体 → **它这一趟的读输入段** 才是点的那一拍（`[0x2214,0x2234)`）——
//   按下落在**回调尾**，抬起要等之后两个 KeyLevel 读点 ⇒ 按下跨过了一个完整回调
//   （这正是 `.plan` 当年 `mouse_down` → 门 → 门 → `mouse_up` 想要的那个形状）。
// · 主轮询体 `[0x1FF4,0x2048)` **不在本表**：`.plan` 用它当"按下已经跨过一整帧"的中间门，
//   而插件这条路上没有"按下 / 抬起"两个可分开的接口（`ctx.input.down/up` 不施加 ——
//   `src/runtime/intent_executor.cpp` 的 `MouseDown`/`MouseUp` 是 C1 的不施加档）
//   ⇒ 中间门没有消费方 ⇒ 按本文件头的收录判据不导出。
// ===========================================================================

// 回调体（入口 → 它那一段输入读完）：目录屏就绪 = 右键那一步的**前置**门（不是点的那一拍）。
export const REIGN_CALLBACK_LO = 0x2048;
export const REIGN_CALLBACK_HI = 0x2234;

// 回调体尾的**读输入段**：★ 右键就在这一拍提交（见上：让按下跨过回调边界；实测读数见消费方文件头）。
export const REIGN_READ_INPUT_LO = 0x2214;
export const REIGN_READ_INPUT_HI = 0x2234;

// ===========================================================================
// SETTOOLLIST.BIN（无 doc；**没有**取证过的位置区间）
//
// 它在复现链里只当**脚本身份**用（切换的转场：`click` → `SETTOOLLIST` → 回 `FELLOW`），
// 走 `afterScript`，不需要 `pos` ⇒ 本表**不收**它（⛔ 也不要为了整齐编一个区间）。
// ===========================================================================

// ===========================================================================
// SELACT.BIN（无 doc；读数同上）
// ===========================================================================

// 主轮询体（`get-input-type` + `sleep 1` + `jmp 0x834`）= 「決定」那一屏
//（实测：结束回合后 ~3 s 在 0x44EC、~13 s 落到 0x0870）。
export const SELACT_MAIN_LOOP_LO = 0x0834;
export const SELACT_MAIN_LOOP_HI = 0x08c0;

// ===========================================================================
// BTL.BIN（无 doc；读数 = `save07-battle.plan` ⑧ / 清单行 975–977）
// ===========================================================================

// 战斗主循环（`poll-input` + `wait`）：等主循环 = 等"加载完、战斗在跑"。
export const BTL_MAIN_LOOP_LO = 0x4a84;
export const BTL_MAIN_LOOP_HI = 0x4ae4;
