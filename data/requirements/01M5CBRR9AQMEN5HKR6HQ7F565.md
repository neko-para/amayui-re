# 🐞S2 spec-isa 把历史汉化 _extracted 当安装语料 ⇒ 无该目录的机器 assets 永远红

- id: REQ-01M5CBRR9AQMEN5HKR6HQ7F565
- type: bug
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- verify: tools/test/spec-isa.assets.test.mjs#★ 语料侧
- repro: tools/test/spec-isa.assets.test.mjs#★ 语料侧
- severity: S2
- tags: [spec, test]

## 现象

`tools/test/spec-isa.assets.test.mjs` 的 `collectScripts()` 把 `<gameInstall>/_extracted/**/*.BIN` 也算进语料。
`_extracted` 是**历史汉化版本在旧仓里留下的临时解压（overlay 产物）**，不是安装数据（`docs/01-translation/patch-design.md` 已把它列为 overlay 排除项）。
后果：

- 有 `_extracted` 的机器：结构断言量到**另一份文件**（表 3 之后多出一截未解释字节）；
- 没有它的机器：顶层只有散装 105 支 ⇒ 脚本数 < 400，**assets 永远红**（本机就是这一档）。

## 修法

- `collectScripts()` 只收**安装根顶层** `*.BIN`；`_extracted` 不参与；
- 语料下限从 400 改为 50（只防"根指错 / 目录空"，不写死具体数）；
- "第 3 张表结束 == body 末尾"对**全部顶层脚本**生效（原来只对顶层，现在没有 nested）；
- spec 侧同步：`10-format` 的「body 与表指针」升为**已核验**；`audit.json` 的 `RES-ISA-TABLES-EXTRACTED` 移出（它不是安装语料的残差）。

## 判据

```bash
node --test --test-isolation=none tools/test/spec-isa.assets.test.mjs   # 本机（无 _extracted）绿
pnpm test:assets                                                        # 失败从 8 降到 2（只剩 corpus 本机写入口）
```

★ 本机实测：顶层 105 个 `.BIN`，脚本 104 支；表尾不匹配 0。
