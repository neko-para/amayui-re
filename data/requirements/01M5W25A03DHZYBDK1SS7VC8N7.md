# 残差第三批：释放守卫写者 · 存档恢复同步

- id: REQ-01M5W25A03DHZYBDK1SS7VC8N7
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- done_reason: 守卫仍 unknown（无写者、0xB670 未初始化）；0x216 存档恢复已由读档逐项 sub_4A3800 重建活动表 0x748 收口。bindings 补 0x4113ed；spec/req/ledger/test/ctest 全绿。
- tags: [spec, residual]

## 结论

1. `RES-TEX-GUARD-WRITER` → **unknown**：全语料只有 0x1fa / 0x23d 两处读（`0x49e98a`），没有写点；`sub_499BC0` 只初始化 0x748/0x5568，不写 0xB670。它是否依赖零页 / 宿主写仍未取证。
2. `RES-216-SAVE-RESTORE` → **resolved**：读档 `sub_410160` 逐项调用 `sub_4A3800`，后者写活动表 0x748 的来源文件格；因此 `0x216` 读到恢复后的活动表。保存侧 `sub_40CD10` 从快照写档，与读档重建不矛盾。

## 判据 / 证据

- `0x4113ed`：`sub_410160` 内 `sub_4A3800(Scene, fileId, handle, id, color, 0)`。
- `0x49e98a`：守卫唯一读点；0xB670 无写点。
- C++ `check_slot_imgid` 覆盖 0x216 缺省 / 守卫 / 0x23d 边界。

## 下一步

- 守卫：查 Engine 分配 / 零初始化与宿主 / 渲染路径；确认无写者则登记恒 0 / discarded。
