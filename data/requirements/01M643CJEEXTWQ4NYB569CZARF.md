# 纹理记录与纹理槽在模型里合成一张表（0x1fa 删记录，spec 说保留记录只清槽对象）

- id: REQ-01M643CJEEXTWQ4NYB569CZARF
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#0x1fa 把纹理**记录**删掉了

# 纹理记录与纹理槽在模型里合成一张表（0x1fa 删记录，spec 说保留记录只清槽对象）

- id: REQ-01M643CJEEXTWQ4NYB569CZARF
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#0x1fa 把纹理**记录**删掉了

## 分歧（已消失）

spec 的 `0x1fa` 说：先清**共享对象槽**里该 id 的对象（不受释放守卫影响）；释放守卫为 0 时把
**纹理记录的来源文件**写成"无"、清**纹理槽里的对象**；★ 记录本身**保留**（颜色键 / 空白标记不动）
⇒「空槽 ≠ 空白纹理」是两种状态。

旧实现把「纹理记录」与「纹理槽」**合成一张 `textures_` 表**，`0x1fa` 直接 `textures_.erase(id)`
把记录整个删掉；同一编号重建时版本号从 1 重来 ⇒ 窗口路径的 GPU 纹理缓存按 `(id, revision)` 命中，
把上一条记录的纹理当成当前纹理（缺陷单 `REQ-01M642804D7FRDHYFFRZ03C9ZH`）。

## 修法（重构，不是打补丁）

`apps/emulator-cxx/src/core/vm.h`：

- `TextureRecord`：只剩记录字段（来源文件 / 颜色键 / 空白创建 / 非文件装载）。
- `TextureSlotObject`：槽里的对象（`present` / 宽 / 高 / `revision` / 像素）。
- `TextureEntry { record, slot }`；`textures_` 的类型换成它。

读口分两半：`texture_record(id)`（永远能查到记录，`0x216` 用它）、`texture_slot(id)`
（记录在但**槽已释放 ⇒ nullptr**，`0x208` / 绘制 / GPU 上传用它）。写口 `texture_mut`（整条）、
`texture_entry`（建/取）、`next_texture_revision`。

行为变化点（逐条）：

1. `0x1fa`：`record.file = -1` + 清槽对象（`present=false / 宽高 0 / 像素空`），**不删条目**。
2. `0x1f8`：只改"来源 = 无 / 空白创建 = 1"与槽，**颜色键保持原值**（拆开后才有地方保）。
3. `0x1f9` / `0x1af`：整条替换（记录 + 槽）。
4. `0x23d`：同 `0x1fa` 口径清槽 42..999 的对象，记录只写来源 = -1。
5. 场景命令族 `0x223 / 0x24f / 0x250 / 0x251` 的"没有就建 1280×720 画布"改为按**槽对象**判空
   （`ensure_scene_texture_slot`）—— `0x1fa` 之后记录还在、槽空 ⇒ 照建（与旧"删整条再建"等价）。
6. `0x24d` 的"影片纹理槽为空"门也改判**槽对象**。
7. `next_texture_revision` 自己把条目当前 `slot.revision` 算进下限（文本绘制 / 场景效果走
   `slot.revision++`，不经过它；只信下限会发出一个用过的版本号）。
8. `texture_count()` / `textures()` 的口径随之变成"条目数"（含空槽条目）。

## 守卫（CTest `emulator-core-selftest`）

新增/强化（`apps/emulator-cxx/tests/core_selftest.cpp`，同一段）：

- `0x1f8` → `0x1fa` 之后：`texture_record(7) != nullptr`（删整条 ⇒ 红）、`file == -1`、
  `blank` 保持 1、`texture_slot(7) == nullptr`。
- 同一时刻 `0x208` 必须回 **0/0**（记录在、槽空）；重建后回 64/32。
- `0x1fa` → `0x1f8` 重建：`revision > 1`（缓存不命中旧内容）。

**变异实测**（本轮）：

- 把 `0x1fa` 改回 `textures_.erase(id)` ⇒ 红：
  `0x1fa 把纹理**记录**删掉了：spec 只清槽里的对象、记录（来源 / 颜色键 / 空白标记）必须保留`。
- 把 `0x1f8` 的 `revision = next_texture_revision(id)` 改成 `= 1` ⇒ 红（先命中两轮重建那条）：
  `两轮「重建 + 同条数文字」后纹理版本号 = 2（≤3）…`。

## 归因

**实现偏离 spec**，不是 spec 缺陷：spec 的两层描述本来就是精确的（记录 / 槽对象 / 空槽 ≠ 空白纹理），
上一版实现把两层合成一张表并按"删记录"落地。⇒ spec 本轮**零改动**。

## 仍欠

- 共享对象槽（`0x1f8` / `0x1f9` / `0x1fa` 都"先清共享对象槽"）在本模型里没有独立载体；它是否
  可观察仍未取证（现有实现只在释放守卫支路里体现差异）。
- `0x1f9` 失败路径（解码失败 / 上传失败）记录字段的细节仍按旧口径，未在本次重构里重核。
