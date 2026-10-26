# 残差：0x216 槽→文件绑定 + 0x23d 解绑 / 0x32b 内部释放

- id: REQ-01M5W02JHW3WEP1EBVK4P3FT0M
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/texture-slot-imgid-contract.test.mjs#★ 0x23d：只清 42..999 的槽→文件绑定
- tags: [spec, residual]

## 交付

- 新增 `CORE-OPS-TEXTURE-SLOT-IMGID`（0x216）：读纹理记录的**来源文件格**，缺省 -1；`0x1f8` / `0x1f9` / `0x1fa` / `0x23d` 是写者。
- `0x23d` 的 `RES-23D-SCENE-UNBIND` 收口为「对 42..999 的纹理记录来源文件格写 -1」；图像对象释放闸另立 `RES-23D-IMAGE-OBJECT`。
- `0x32b` 的 `RES-32B-MESH-RESOURCE-RELEASE` 改为 `omitted`：内部渲染资源释放不建模，脚本可见的槽表由合同/守卫固定。

## 判据 / 复现

- `0x216` 缺省 / `0x1f8` 后写 -1；`0x1f9` 成功设置文件 1234 后读回 1234；`0x23d` 后 42..999 读回 -1、41 保留。
- C++ 守卫 `check_slot_imgid`；pure 合同 `texture-slot-imgid-contract.test.mjs`。
- `0x32b` 的 omitted 证据绑 `exit-cleanup-contract`（槽 0..999 清空、负键/1000 保留）。

## 残差

- `RES-23D-IMAGE-OBJECT`（unknown）：图像对象释放闸与释放后槽状态未建模。
- `RES-216-OUT-OF-RANGE`（unknown）：槽号越界后果未取证。
