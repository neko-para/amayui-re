# 0x23d / 0x32b 退出清理：spec 未成篇

- id: REQ-01M5VV50B4PJYC3QWK466H0CZ6
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/exit-cleanup-contract.test.mjs#★ 0x23d：清影片槽 42..999，保留 0..41 与 1000
- tags: [spec, exit]

## 缺口

`0x23D` / `0x32B` 的 C++ handler 已按观察接上（退出路径不再停），但 spec 尚未成篇：`audit.gaps[]` 仍把二者列在 `GAP-OPS-0200-02FF` / `GAP-OPS-0300-03FF`。

## 判据

- 写 `CORE-OPS-MOVIE-CLEAR-23D` / `CORE-OPS-SCENE-MESH-CLEAR-32B`（合同 + 残差 + bindings 站点）。
- 把两个 opcode 从对应 gap 的 `ops` 移出、更新 note/residual 计数。
- 证据锚：`sub_41A300` @ `0x41A300`、`sub_41A4A0` @ `0x41A4A0`；C++ 守卫 `check_exit_cleanup`。

## 备注

C++ 当前实现是退出路径的最小清理：`0x23D` 关/删槽 42..999 的影片对象，`0x32B` 清 0..999 网格槽；场景侧解绑 / D3DX 内部释放记残差。
