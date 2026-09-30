# 批 M1 · 只读语料入位（反汇编语料 + 存档样本）

- id: REQ-01M3TCJB50Z0NW5GY0X36F11ZR
- type: req
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 20
- verify: tools/test/corpus-manifest.test.mjs#LFS 一致性：真仓的 .gitattributes

## 判据（已完成）
反汇编语料 4 文件（UTF-8 + LF）打成 zip 走 LFS；8 个存档样本走 LFS，文件级事实（槽定位 / mtime）落在
`corpus/fixtures/samples.json`；LFS 一致性由守卫 #6 机械核对（不靠登记来源枚举）。

## 凭据
`pnpm tools corpus validate`（#6）与 `pnpm tools disasm verify`（4/4）。
