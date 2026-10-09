# `binary/agerc-modified-install` 的 sha256 pin 指向旧仓里的旧产物（5f3189…），与 v1.14 发布包/入库件的 6241de… 不符

- id: REQ-01M4GE4HSJGZ4XF9BSTZFHKXKG
- type: bug
- status: open
- parent: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- repro: corpus/assets.json#binary/agerc-modified-install
- tags: [corpus, release, assets]

## 现场（实测，2026-10）

`pnpm tools corpus validate` 的 #4（校验和纪律）报红：

```
binary/agerc-modified-install: sha256 与盘上不符
  清单 6241de666d2f… / 实际 5f318955f605…  → install/AGERC.DLL
```

把三份摆在一起就清楚了：

| 件 | sha256（前 12） | 说明 |
|---|---|---|
| **v1.14 发布包**（`/Users/nekosu/Downloads/v1.14-261005/AGERC.DLL`） | `6241de666d2f` | 最新一次发出去的那份 |
| 本仓入库件 `corpus/assets/agerc/AGERC.DLL`（条目 `binary/agerc-dist`） | `6241de666d2f` | **与发布包逐字节相同** ✓ |
| 旧仓 `install/AGERC.DLL`（条目 `binary/agerc-modified-install` 的 pin） | `5f318955f605` | **更旧的一次构建** |

⇒ 红的原因不是"入库件坏了"，而是那条 **pin 指向旧仓测试安装树里的一份旧产物**；
而 `release` 实际发的是 `corpus/assets/agerc/AGERC.DLL`（= 发布包那份）。

## 要怎么收口（二选一，要人定）

1. **刷新 pin**：把 `binary/agerc-modified-install` 的 `sha256` 改成当前发布件的那份
   （= 承认"旧仓 install 树是历史"，pin 以发布包为准）；或
2. **退役该条目**：如果那份 pin 的用途只是"当年用来核对入库件来源"，而来源现在已经有更权威的
   参照（v1.14 发布包 + `release` 的实际产物），就把它标成 `superseded` 并写清理由。

★ 为什么不在本轮直接改：那是**知识层的归属判断**（"pin 该指谁"），不是机械可判的；
本单只把事实与两个选项摆出来。★ 顺带确认：字体两份也与 v1.14 发布包**逐字节相同**。
