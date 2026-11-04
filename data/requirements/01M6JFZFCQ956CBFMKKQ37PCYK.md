# PE 载入对坏节 / 坏导入静默跳过（半载入仍返回 true）

- id: REQ-01M6JFZFCQ956CBFMKKQ37PCYK
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/pe_loader.cpp#section-raw-out-of-range
- repro: apps/emulator-lift/tests/pe_loader.cpp#section-raw-out-of-range
- severity: S2
- tags: [emulator-lift, m1, loader]

## 一句话
PE 载入对**坏节 / 坏导入静默跳过**：`mapPeImage` 在 `raw_offset + copy` 越界时直接不拷贝（半载入仍返回 true）；导入表解析对 `rvaToOffset` 失败 / 坏 thunk 只 `continue/break` ⇒ A5 的"缺口 = 0"可能假绿（缺口被解析器吃掉，IAT 保留 PE 原值）。

## 证据
- `apps/emulator-lift/src/runtime/image.cpp` 的 `mapPeImage`：`if (copy && s.raw_offset + copy <= img.bytes.size())` 之外没有 else；越界静默跳过整节。
- 同文件导入表循环：`if (!rvaToOffset(img, thunk_rva, &thunk_off)) continue;`；`readAscii` 失败 / thunk 越界只 `break`。
- A5 的静态缺口来自 `importedButUnimplemented()`；解析器少读的导入不会出现在这个集合里。

## 验收
- 两处都改成"带原因返回 false"（拒绝启动），与 sha256 不符同档；
- 加一条合成 PE 的 `loader` 夹具（坏节 raw range / 坏 thunk）证明不会半载入；
- 变异条目：把 else 改回静默跳过 ⇒ 夹具红。

## 证据等级
审计报告 `.tmp/m1a-audit/A-runtime.md`（F7/F8）；只读、未运行。
