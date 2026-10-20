# `config-keys.json` —— 配置键词表（spec 的伴随数据）

> ★ 本文件是 `data/spec/format/config-keys.json` 的说明书。字段 / 枚举 / 不变量**只信**
> `pnpm tools spec describe`（`files.configKeys`）；本文件不复述 schema。

## 它是什么

引擎内建的**配置键固定词表**，是 `CORE-CONFIG` 三条输入通道之间的唯一映射真源：

- `defaults`：索引尾部发行默认表的**大写键** → 配置库内部键 + 值类型（整数 / 字符串 / 列表），以及少数键的派生写（`extra`）与特殊处理（`special`）。
- `ini`：玩家文件 `SYS4REG.INI` 的**段名 · 键名** → 配置库内部键 + 值类型。段名是小写、键名是驼峰，匹配按字节精确。
- `persist`：保存时写出的段 / 键的**固定顺序**，以及少数键的条件（`if` 指向另一个内部键）。

它是**引擎代码的一部分**，不是游戏数据：键名、类型、默认值来源都不随某份存档改变。默认值本身来自索引文件，不在这份表里。

## 非显然的口径

- `internal` 是配置库里的规范键；`defaults` / `ini` 两套输入都归一到它。
- `kind: "list"` 的键只追加，不能按元素覆盖。
- `special: "saveVersionSplit"` 的 `internal` 只作占位；实际写两个键（主 / 次版本）。
- `special: "ignore"` 的键在词表里但**不产生任何键**（接受即忽略），不是拼写错误。
- `persist[].if` 是**条件写出**：该内部键为 0 / 空时整条不写。
- 表里没有的键：默认表侧 ⇒ 装载失败；玩家文件侧 ⇒ 忽略。

## 怎么查

```bash
pnpm tools spec describe          # 字段 / 文件清单（schema 唯一真源）
pnpm tools spec show CORE-CONFIG-DEFAULTS
pnpm tools spec show CORE-CONFIG-PLAYER
pnpm tools spec show CORE-CONFIG-PERSIST
```

## 怎么改

- 先改**规范正文**（`data/spec/chapters/55-config.md`）里的行为，再改这张表；两处不一致以正文为准并补证据。
- 改完跑 `pnpm tools spec validate`；键表只被 `apps/emulator-new` 消费，跑 `pnpm typecheck` 与
  `node --test tools/test/emulator-new-config.test.mjs`。
- 新增键必须同时给出它在哪条规则的覆盖范围内；没有对应规则的键不要加。
