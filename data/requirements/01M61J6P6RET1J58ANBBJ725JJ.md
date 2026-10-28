# 文字亮度：配置根（本地/漫游）+ AA 默认值 + 17/65 级换算（实测 max_alpha=240）

- id: REQ-01M61J6P6RET1J58ANBBJ725JJ
- type: req
- status: done
- parent: REQ-01M5Z5D786WF65YWHFK7K8Z7Q8
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-glyph-alpha

## 结论（2026-10-28 收口）

设置界面文字在模拟器里是**纯白**、真机偏暗。缺口有**四条**，各自独立、都已在代码里落地：

1. **配置根读错**（主因）：玩家文件被从漫游根读 ⇒ 玩家层整份没进配置库，AA 开关拿到 0。
   已改成引擎的本地根，见缺陷单 `REQ-01M61K6G0K83P9ZHG073H6BX0R`（守卫 `emulator-core-config-root`）。
2. **全新档的 AA 子开关缺默认值**：它们**不在**发行默认表里，是 `Reg` 构造体自己登记的两条立即数写
   （`message:UseAntiFont` = 1 @`0x491F59`、`message:AntiFontLevel` = 0 @`0x492A0B`，键名 @`0x491F52` /
   `0x492A04`，upsert @`0x491F63` / `0x492A11`）。已补进 `ConfigStore::register_engine_defaults()`。
3. **宿主回退无视 AA 三格**：FreeType 路径以前恒定灰度、且用 16 / 64 级（满覆盖 255）；引擎是
   **17 / 65 级**（`0x46DAAF` / `0x46DA9C`；32 位表面支同名算式在 `0x46DCFF` / `0x46DCEC`）⇒ 满覆盖
   **240 / 251**。现在格式**对所有字形来源**按 `core::glyph_format` 走，级换算在 `core/text_alpha.h`。
4. **空画布上的首次写入被写成了"与黑色加权"**：引擎有**两条**整像素覆写支 —— 满覆盖（`0x46DDC6`）
   与**目标 alpha == 0**（`0x46DDD5`，刚清空的文本层上每个像素的第一次写入都走它）；
   两支都是"源色原值 + 覆盖值"。我们以前只有满覆盖那一支，其余一律加权 ⇒ AA 边缘的 RGB 被压成 α 倍
   （GRAY4 满覆盖 94%、边缘更狠）⇒ 字更细更暗。`compose_texel` / `compose_glyph_texel` 两支都补齐了。

## 实测判据（不是推理）

- 设置界面示例文本那层画布（纹理 29，824×120）导出扫描：**`max_alpha = 240`、`opaque(=255) = 0`、
  非零 alpha 的**取值只有 16 种**（15,30,…,240 —— 正是 17 级 GRAY4 的非零集合）**。
  （带阴影的示例文本里另有一圈纯黑+同一批 alpha 的像素，那是副色趟，不是主色被压暗。）
- 四处变异各自能让守卫当场红（都已实跑）：
  - `kGray4Levels: 17 → 16` ⇒ `GRAY4 满覆盖应当是 240…得 255`；
  - 回退路径 `quantize = false`（= 旧行为）⇒ `AA 开（GRAY4）时最大 alpha 应当是 240，实际 255`；
  - 去掉 `dst_alpha == 0` 那条覆写支（= 旧行为）⇒ `墨迹 RGB 被压暗了（最小通道 = 15，期望 255）`；
  - 配置根换回 `roaming_app_data` ⇒ `两个候选都在时必须选**本地**应用数据目录`。

## 守卫

- `apps/emulator-cxx/CMakeLists.txt#emulator-core-glyph-alpha`：格式选择（两开关都要非 0、级数 1 才是 8 位）、
  17 / 65 级换算（GRAY4 的 alpha 必是 15 的倍数、上限 240；GRAY8 上限 251）、端到端那条
  "AA 关 ⇒ max alpha 255 / AA 开 ⇒ 240 且墨迹 RGB = 源色原值"。

## 还剩什么（不在本单）

- spec 的 `RES-RENDER-TEXT-GLYPH-FORMAT`：AA 两格的**写点 / 生效时机**（构造抄一次？重建时重读？）仍未定；
  本单只钉"读到的值怎么变成覆盖值"（已验证与二进制一致）。
- `0x82` 的颜色看起来是"借用后归还"（`sub_466000` 尾部把旧值写回）—— 单开
  `REQ-01M61KBHZJ58HMXTH93DZHXJ09`；它是**颜色残留**问题，不是 alpha。
