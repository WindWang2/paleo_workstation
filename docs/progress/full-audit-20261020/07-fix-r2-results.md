# Review 2 结论与 Fix 2 处置（full-audit-20261020）

R2 复审者：独立 subagent（未参与修复），报告 /tmp/paleo-audit-r4/review-r2.md。
R3 终审者：另一位独立 subagent，报告 /tmp/paleo-audit-r4/review-final.md。

## R2 发现与处置

| R2 意见 | 分级 | 处置 | Commit |
|---|---|---|---|
| MF-1 D-13 新测试第 4 断言必红（tooltip 残留）+ 产品语义只修一半 | MUST_FIX | 无建议路径复位默认 tooltip（早退分支） | 62799a48 |
| MF-2 seismic3dviewpanel.cpp 1864 > 冻结值 1827（闸门红） | MUST_FIX | setVolume 拆出 seismic3dviewpanel_volume.cpp（主文件 1775） | 573e23a9 + 22f82891 |
| SF-1 渲染器 GPU 侧堆叠层无清除入口（新体取数失败旧体平面残留） | SHOULD_FIX | ClearStackLayers（renderer+viewport+setVolume 调用） | 573e23a9/d1f293da |
| SF-2 recycle「removed」schema 解析三份拷贝 + 少空 assetId 防御 | SHOULD_FIX | 登记 OPEN（P3，下沉 services 单一事实源属架构小批次，未修） | — |
| SF-3 copyDataset 自清测试未交付 | SHOULD_FIX | copyDatasetFailureCleansDirectory | d3ee62f9 |
| SF-4 B-11 2e4 闸连带拒掉 regionalcontours 大输入（行为变化未点名） | SHOULD_FIX | 登记 DOCUMENT_ONLY（错误文案已给「抽样/裁剪」出路；#149 拒绝优于静默降档） | — |
| SF-5 triage 非 success/failure 一律称「超时」 | SHOULD_FIX | 区分 startup_failure 文案 + 夹具 | e1794752 |
| B-05 u64 回绕疑/ D-13 handler 序疑/ C-15 竞态疑 | FALSE_POSITIVE | R2 自我证伪（goodGrid 界/殊途同归/析构 waitForDone 先行）——C-15 守卫无害保留 | — |

## R3 终审发现与处置

| R3 意见 | 分级 | 处置 | Commit |
|---|---|---|---|
| tst_wellattributes 的 QDoubleSpinBox include 落在未提交工作树（HEAD 不编译该测试；终局回归数字吃脏树） | MUST_FIX (P1) | 补提交（修复本身早在工作树并已跑绿——证据链成立但提交树断裂，终审抓得准） | d45f74d7 |
| volume TU 未用 QComboBox include | SHOULD_FIX | 移除 | d45f74d7 |
| A-10 stored int 截断洞「不可达」等 3 条 | DOCUMENT_ONLY | 登记册备注 | — |

其余挑战（极端输入 5/5、层归属、TU 拆分 ODR、基线零违约、仓库卫生）均未推翻。

## 修复后状态

- 提交树完整编译（build13）+ tst_wellattributes 单例复跑绿。
- 终局回归（426/440，14 红全为 master 既有/在册/负载抖动）在含 include 的
  工作树上取得；提交树与该工作树在测试行为上等价（include 只影响编译完整性）。
