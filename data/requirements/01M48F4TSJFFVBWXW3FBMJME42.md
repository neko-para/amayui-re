# local 池基址几何与旧仓说法不一致（local_float 的帧+0x38 在语料里零次出现）

- id: REQ-01M48F4TSJFFVBWXW3FBMJME42
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 13
- verify: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何：**6 个整齐基址**
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何
- severity: S2
- tags: [engine, emulator, ledger]

# 🐞S2 local 池基址几何与旧仓说法不一致（`local_float` 的基址在 `帧+0x84`，不在 `+0x38`）

- id: REQ-01M48F4TSJFFVBWXW3FBMJME42
- type: bug
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 13
- tags: [engine, emulator, ledger]
- severity: S2
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何
- verify: tools/test/emulator-model.test.mjs#★ local_float 的基址在 `帧+0x84`

## 分歧与裁决
* **旧仓口径**（`analysis/fields.json` ScriptContext）：基址 `帧+0x34..+0x48`（两组各 6 个、整齐对齐）。
* **实测（已裁决）**：基址 **6 个但不整齐** = `{+0x34 int, +0x84 float, +0x3C string, +0x40 ptr, +0x44 floatPtr, +0x48 stringPtr}`；
  ★ `+0x38` 在整个语料里**出现 0 次**（`5D8B8h`），旧仓在那里的那一格是**空的**。
* 配对方法（**不按顺序猜**）：装载器里每次 `operator new[]` 之前那条**计数槽加载**与**元素宽度**，
  配对该次分配之后的**落点写入**。实测 7 次：4B/4B/28B/4B/4B/4B（六池）+ 1 次属另一族（三数组那一路）。

## 影响（为什么不是小事）
模型若照旧仓填 `+0x38`，`local_float` 会指向语料里**从没人碰过的位置** = 编造事实；
而真正的 float 池（`+0x84`）会被当成 `array_container`（旧仓也把它记成了 std::vector）。

## 残留
* 装载器第 7 次 `new[]`（`.text:0040F5AC`，计数槽 `帧+0x20`、写入 `+0x5D8F8h`）与 `+0x84` 是否同一个池的**第二份**，
  以及 `0x5D8F8/0x5D8FC/0x5D900` 三个 cur 索引数组的用途 —— 仍由 `REQ-01M48EVKSNX139B8TX0CYXZ0A1` 跟踪。
* `frame+0x84` 与旧仓记的 `array_container`（std::vector）**不是同一个东西**该结论需要另立一条核实。
