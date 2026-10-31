# 字体与模式位：名字表 · Font+0x548 · 键表 0x818 · A/C 两格 · 视口分量

- id: REQ-01M68PPHW1VC5MK3BQD8GPMDBW
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 残差清单式节点：audit.json 的残差已带 what/next，需求树不重复登记；无 audit 归宿的条目已并入 REQ-01M694VKD97ERC5ZBHDA4AHQV2。
- tags: [spec residual]

## 目标

收敛字体对象 / 模式位 / 键表这一族在批 2 留下的缺口。

## 具体项

1. **字体对象的两份名字表**（`Font+0x18DC8` / `+0x30F2C`）：两份描述各自的字面名表内容与超长名处理（`strcpy_s`）。
2. **`Font+0x548` 的载体一致性**：57 处访问、45 处 `cmp …,0`、唯一写者构造器写 0 ⇒ 疑似不可达分支；要证"载体是否一致 + 是否被整块拷贝"。对应残差 `RES-OPS-FORWARD-PROLOGUE-BIT2` 的剩余部分。
3. **键表 `0x818` 的读者与键掩码 `+0x1620` / `+0x1624`**：`0x10c` 的键→动作表读者、二级下标的定义域。`0x1668` 槽表的容量与初值也未定。
4. **门位 27 服务体里 5 个调用的领域**：服务体 `0x4133A3` → `sub_411900`（消息步）之后的 5 个调用各自做什么。
5. **`0x19b/0x19c/0x1c7` 的 A / C 两格按读者定域**（A 有 2 个读者、C 有 9 个读者）：对应残差 `RES-OPS-MODE-BIT-ROLE` 的剩余部分。
6. **视口 / 客户区矩形的「宽, 高」排列**：对应残差 `RES-OPS-FONT-SCALE-COMPONENT-ORDER`（读 `0x46584E..0x46588A` 的两个分量来源，若相反就互换正文两句与 bindings 的两条 `what`）。
