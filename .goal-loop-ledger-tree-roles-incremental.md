# 方向 92 ledger：数据树角色词表单源化 + 树增量通道

基线：origin/master 753c2768（任务文行号按 32851a1e 口径；四目标文件在两提交间仅
datacatalog.h 有 prepareOpen 无关新增，勘察按实际行号进行）。

## R0 勘察结论（定案）

1. **词表现状**：`RoleRegistry`（src/catalog/roleregistry.h/.cpp）已是链接角色的
   工程词表描述面（display/entityTypes/ordered，project_area.json 可覆盖）——但它
   不含 `reference` 角色、无树排序序、无分组族。`catalogRoles()` 定位为**树/AI
   消费面的常量词表**（role→{group, order, display}），与 RoleRegistry 互补：
   RoleRegistry 管工程准入/描述，catalogRoles 管树序/分组/AI 清单。
2. **显示名口径（无人值守定案）**：树已在渲染的四个角色显示名以**树面现状为准**
   （tops→井分层、core→岩心照片，与 RoleRegistry.display 的 分层顶/岩心 有历史
   分叉）——等价重构红线优先。树未渲染的角色沿用 RoleRegistry 显示名。分叉记
   PR body（Low）。
3. **i18n**：paleo_zh_CN.ts 为恒等翻译（zh→zh 同文），tr() 换数据层裸串不改运行
   时文本；check_i18n 只扫 setter 首参裸字面量，setText(0, 变量) 形态不命中。
4. **AI 面**：tst_aichatprojectquery 不断言 role 参数描述文本（只断 schema/enum/
   dispatch 出参）——描述可安全改为 catalogRoles() 生成；resultSchemaJson 不动。
5. **树增量（R0 择一：QTreeWidgetItem 就地增删，不模型化）**：理由——DataNavTree
   携带拖放/激活/过滤语义，模型化会牵动 selection model 与 DnD 路由；就地增删
   以最小面保 #270 语义（不 clear 即无丢失）。
   - 中间层 `NavNodeSpec`（纯数据蓝图）：buildNavTreeSpec() 从 catalog+面板状态
     产出；全量重建与增量 reconcile 共用同一 apply 代码——**内容等价由构造保证**。
   - reconcile：按身份键（type|id|eid|sub，无 id 节点用去计数文本，与
     navExpandedKey 同 trick）匹配，无匹配→增/删，匹配→字段全量重放（含清除面：
     颜色/字体/flags），纯位移不计变更。
   - 兜底阈值（无人值守定案）：inserts+removes+updates > max(16, 总节点数/4)
     → 全量重建（快照展开+clear+重建+还原，现行 #270 路径原样保留）。
   - 过滤重放不需要：refreshAssetTable → applyListFilter → renderAssetPage →
     refreshAssetTree 之后，datalist_filter.cpp:102 applyFilterToTree 总会重放。
6. **未分组桶**：词表外角色（工程自定义 role 等）在井节点下落「未分组 (N)」可见
   分支（沿方向 79 岩心照片分支先例）；默认词表下不出现（等价对拍零差异）。
7. **mapproduct 族**：当前无链接角色属成果图件族（该组按资产类型归集）——族键
   预留在组表并文档化，空族不进 AI 描述生成。

## 环境与基线定案（R0 迭代记录）

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|----|------|------|------|--------|
| R0 | worktree + 勘察 | 见上 | ✅ | 批1 词表 |
| R0a | 基线构建（origin/master 753c2768） | ninja 失败：paleomainwindow.cpp:650 C2027（#315 加 cancelPreviewData 调用未带 include；修复 efbcd762 在他人分支未合入） | ❌ 上游破损 | 换基线 |
| R0b | 基线回钉 32851a1e（任务行号口径；四个目标文件与 HEAD 内容一致，diff --stat 证实） | git reset --hard + stash pop 干净 | ✅ | 构建 |
| R0c | 环境注记：① ps1 须用 pwsh 7（PS5.1 按 ANSI 误读无 BOM UTF-8 脚本）；② PATH 须前置 C:/Qt/Tools/CMake_64（VS 自带 3.27 < 3.28）；③ 本机 8 并行在 paleo_ui 大 TU 处编译器静默被杀（疑似内存压力）——PALEO_JOBS=2 稳定；④ 构建中改源文件会污染在跑构建（R0 教训） | - | ✅ | 开发构建 |

**R0 红集合来源定案**：主 checkout（本地 master=32851a1e，树净）的 build 目录同链已配置——终局对照在主 checkout 跑全量 ctest 取 R0 红集合，worktree 跑改后全量 ctest，diff 为空即验收。

## AI 描述改前/改后对拍（Oracle 4 证据）

- 改前（手写串，domaintools.cpp）：`关联角色过滤（well_head/well_log/tops/time_depth/horizon/seismic_volume/reference 等），作用于 well_details`
- 改后（catalogRoleListForAi() 生成）：`关联角色过滤（词表角色按分组：井：well_log/tops/time_depth/well_head/core/trajectory/cuttings/lab_analysis/interpretation/qc/other；地震：seismic_volume/horizon/geometry/velocity/fault/interpretation/other；辅助：reference/framework_unit/siting_note 等），作用于 well_details`
- 语义对拍：旧串 7 个角色名全部在改后清单中（超集）；分组名（井/地震/辅助）与角色清单同源 catalogRoles()；「等」字保留（工程自定义角色提示）；schema/参数名/enum 零变更。
- 交叉验证：tst_catalogroles::aiListingSkipsEmptyFamilies（清单内容/空族剔除）+ tst_aichatprojectquery 全绿（出参 schema 面）。

## 第一轮 diff 自审（五维）

1. **分层**：catalogroles 归 catalog（数据层，头三行标记 ✅）；树/ai 消费 include 相对路径合法；
   paleo_ai→paleo_store 链接为功能层向数据层依赖（层 DAG 允许，check_layering --strict 绿证实）。
2. **等价对拍**：roleOrder 查表后 0/1/2/3 序不变（tst_catalogroles 显式断言）；四个角色显示名
   逐字面保留（测井曲线/井分层/时深关系/井身/井位）；默认词表下无未分组桶出现（全部 22 角色在表）；
   AI 描述为旧 7 角色超集+分组名（对拍记录见上）；树其余段为逐行搬运（git diff 逐行核对）。
3. **单源**：rg 三处旧硬编码零命中（lambda/手写串/注释权威点全部改指 catalogRoles()）。
4. **增量正确性**：leftover 删除与消费互斥（dequeue 语义）→ 无双删；未匹配子树整删
   （Qt 父删子随）；归位段摘-插序保证 spec 序；新建项展开态插入后施加（无视图项展开丢失坑）；
   applyNavSpec 全量重放含清除面（告色/mono/禁选/tooltip/role4）。
5. **i18n**：tr() 均在视图层；数据层词表 display 为中文常量（与 RoleRegistry 同先例）；
   check_i18n 绿。

发现（Low，记 PR body 不修）：① applyNavSpec 对每次刷新全量 setData/setFont——
微量冗余但保清除面正确性；② ai_dump_test 临时注册项需终跑前 reconfigure 清除。

待办：主 checkout R0 红集合 diff（跑完中）。
## R0' 红集合对照（Oracle 5）

- R0'（32851a1e+errorhub 修复，同链同 ORT）全量：18 红/377。
- 方向 92 后全量：22 红/379（含 ai_dump_test Not Run 注册残留 1 项）。
- 裸 diff 9 项差异全在 perf/seismic/errorhub 面（与本方向树/词表/AI 面零交集）。
- **判定（flaky 证据）**：9 项差异测试在 R0' 状态隔离复跑 9/9 全绿——
  归因：① 两次全量跑与其他套件并发（主 checkout 套件同刻在跑）造成负载
  竞争，perf 计时阈值类测试超时；② errorhub_storm/wellsiting_kernel 为
  压力/数值边界类偶发。稳定红集合（两侧共有的 15 项）完全一致。
- 终跑纪律修正：终跑独占机器（无并发构建/套件）。


## stash 事故记录（复盘）

- 教训：`git stash pop` 两次连用——第一次 pop 后编号位移，第二次 `stash@{0}` 抢到了
  共享仓库里其他会话的 wellsection dir98-wip（带冲突，stash 本体保留未丢）。
- 恢复：untracked stash 被弹丢的 4 文件从 fsck unreachable 提交 93089594 提取回盘；
  ledger 以 stash 版为底 + 追加段接回；wellsection 污染（staged/冲突/5 新文件）全部
  清除——dir98-wip 仍完好躺在 stash@{0}，另一会话无损失。
- 防再犯：worktree 内不再使用多 stash 连续 pop；改前一律先 commit 到分支。

## 第二轮 diff 自审（终态代码）

1. CMake：paleo_ai→paleo_store 两分支（有/无 ORT）都补——无 ORT 空壳链也解析 domaintools 的
   catalogroles 符号（CI Linux 链同样需要）。
2. datalist.h：TreeRefreshMode/阈值常量为 public 诊断面（测试断言用），NavNodeSpec 机器
   private；m_lastTreeRefreshMode 初值 Unchanged（首次 refresh 前）。
3. 等价面终核：applyNavSpec 重放字段与原构建序逐项对齐（text0/text1/tooltip/UR1-4/icon/
   告色/mono/flags/expanded-仅新建）；restoreTree/treeItemForAsset 在增量模式下照常工作
   （refreshAssetTable 尾部的既有调用不需改）。
4. 无 High/Medium 发现；Low 两项已记（applyNavSpec 全量重放冗余 / 显示名分叉以树面为准）。

## 提交拆分

1. e1a0f26f fix(ui): errorhub include（上游破洞，独立修）
2. feat(catalog,ui,ai): 方向 92 主体——catalogRoles() 单源 + 树增量通道 + 未分组桶 +
   AI 描述动态化 + 词表/增量测试 + MODEL_MAP 4a + ledger

## 终跑定案（Oracle 5 闭合）

- 终跑（独占负载）：13 红/378（97% 过）。
- 三全轮（R0' 18 红 / 并发后 21 红 / 独占后 13 红）稳定核 12 项三侧一致
  （既有机器债：perf 阈值/冷启时序/混链遗留）。
- 全部差异项均有「隔离复跑通过」或「他轮全量通过」证据（9 项批量复跑
  9/9 绿 + tst_coldstart 隔离绿）——负载敏感 flaky，非回归。
- 方向 92 相关面零红：tst_assetpaging / tst_catalogroles / tst_aichat /
  tst_aichatprojectquery / tst_dataops_incremental / tst_panels / tst_ui（独占轮）
  全绿。

## 完成协议（验证跑两遍）

- 二次重跑：目标五测试（catalogroles/assetpaging/aichatprojectquery/dataops_incremental/aichat）
  全绿；layering 严格+非严格、check_i18n 绿；三处旧硬编码 rg 零命中
  （importrows.h 命中项为 CatalogAsset.type 分类器类型注释，非链接角色词面）。
- 提交：e1a0f26f（上游 include 破洞修复）+ 653e47d8（方向 92 主体）。
- 终态：13 红/378 独占轮 + 12 项稳定红核三侧一致 + 全部差异项隔离绿。

## 遗留（Low，移交 PR body / 后续方向）

1. applyNavSpec 全量重放冗余（换清除面正确性）——大树上可优化为字段级 diff。
2. tops/core 显示名与 RoleRegistry.display 历史分叉（分层顶/岩心 vs 井分层/
   岩心照片）——以树面现状为准；未来统一时改 catalogRoles() 一处即可。
3. 本机 8 并行编译在 paleo_ui 大 TU 处静默被杀（PALEO_JOBS=2 规避）——机器
   内存配置问题，与 BUILDING.md「文件监狱」同类，移交用户决策。
4. 上游 master 753c2768 MSVC 非 unity 链不可编译（#315 缺 include + 本基线
   errorhub 同病）——修复 efbcd762 在他人分支，合流后自消。
