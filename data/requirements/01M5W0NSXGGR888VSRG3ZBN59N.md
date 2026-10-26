# 残差第二批：0x216 越界 · 0x23d 图像对象闸

- id: REQ-01M5W0NSXGGR888VSRG3ZBN59N
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- done_reason: 0x216 越界改为 unknown（1000..1999 快照别名，本版只保 0..999，C++ 响亮失败）；0x23d 纹理槽清空 + 来源 -1 入合同，图像内部释放 omitted；守卫与 0x1fa 同一字段，写者残差合并。check_slot_imgid + texture-slot-imgid-contract；spec/req/ledger/test/ctest 全绿。
- tags: [spec, residual]

## 结论

1. `RES-216-OUT-OF-RANGE` → **unknown**：0x216 没有范围比较；`1000..1999` 会落到紧邻的快照表，`>=2000` 继续落到后续内存。本版只保证 `0..999`；C++ `op_216` 对越界响亮失败（`texture-slot-out-of-range`），不复刻别名。`RES-216-SAVE-RESTORE`：存档恢复写的是快照，是否同步活动表未取证。
2. `RES-23D-IMAGE-OBJECT` → **omitted**：图像对象内部资源释放不建模；脚本可见的**纹理槽清空**与**来源文件 -1**写成合同（`0x208` / `0x216` 可观察）。释放守卫与 `0x1fa` 同一字段 / 同一读点；C++ `surface_release_gate_` 分流。
3. `RES-TEX-GUARD-WRITER`（原 0x1fa 残差）扩展为 0x1fa / 0x23d 两处读、没有写者；观察上恒 0。

## 判据

- `check_slot_imgid`：未写入槽 500 读 -1；守卫非 0 保留；守卫 0 清来源与槽；0x208 槽 50 → 0/0；槽 1000 响亮失败。
- `texture-slot-imgid-contract`：缺省 / 1f8 / 1f9 / 1fa / 42..999 边界。

## 残余

- `RES-216-OUT-OF-RANGE`：快照别名是否要写成合同。
- `RES-216-SAVE-RESTORE`：存档恢复是否同步活动表。
- `RES-TEX-GUARD-WRITER`：守卫只有读点、没有写者。
