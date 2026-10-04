# 方向24：交互式约束编辑与地图数字化

基线：`bd1eb0a`（origin/master）；分支：`goal/interactive-editing-20261004`。
依赖：主工作区 vendored superbuild / ONNX Runtime 的绝对路径符号链接。
构建：RelWithDebInfo / Ninja / -j8。主工作区用户未提交改动不动。

## 轮0：复用盘点

- 改动：建立指定 worktree；读取 DESIGN.md、分层契约与 PLAN §34/39/41/42。
- 验证：已有顶点工具（拖动/插入/删除/拓扑）、六种 capture 工具、PaleoUndoStack、编辑条和逐线参数编辑。
- 判定：顶点编辑仅 editBuffer，保存前 store 不同步；逐线参数直写 store 绕过原生 undo；规则形预览为原生折线而非最终形状；缺少批量改型、可配捕捉/栈深、回车/双击结束契约。
- 下一步：事务化 store 快照同步及故障回退，接入原生栈；再补数字化与编辑 UX。

每批须记录构建/受影响测试以及六维全量 diff 自审；收口须连续两轮零 High/Medium。

## 轮1：编辑持久化、数字化与属性接线

- 改动：事务化整层位快照保留 FID/type/params/horizon；约束会话接入原生命令边界、undo/redo、失败回退、完整取消；批量改型/方向角/blockMode；规则形实时预览、回车/双击完成；原生捕捉容差与栈深设置、编辑模式提示和虚线高亮。
- 验证：受影响 23 项首轮 21 绿；tst_constraintstore 原样绿；layering 三项、ui_invariants 三项、i18n 两项均绿。新用例暴露删除后要素计数偏差、测试壳缺失编辑服务；上一批实际拖点精度和延迟销毁预览问题已修复并回归。
- 六维全量 diff 自审：分层无违规；store 一致性发现 High（共享 provider 删除双重扣减、撤销重读变值）；语义发现 Medium（方向角必须旋转真实几何）；资源发现 Medium（场景先销毁后 deleteLater 预览）；DESIGN 使用既有原生控件/字体，无新视觉 token；i18n 新文案进入 TS。以上均进入修复，不能收口。
- 修复：稳定 FID 的临时 GPKG provider 基线，所有编辑仍即时同步正式 store；结束恢复正式源；工程保存 XML 只写正式源；移除逐命令全快照历史，仅保留初始/当前状态并复用原生撤销；标量字段按类型写回，二进制浮点精度不丢；工作副本、OGR 句柄和场景预览均用明确生命周期管理。测试壳首次按应用真实入口完整接线。
- 下一步：全量 -j8 构建、Oracle 回归和两轮零 High/Medium 全量 diff 自审。

## 轮2：集成回归与边界修复

- 改动：列表/表单跟随 undo/redo 和会话结束刷新；loadConstraints 保留活跃工作 provider，避免刷新重声明销毁原图层；非线要素禁止线语义改型（批量预校验，全批拒绝）；约束无同步服务禁编；退出按钮显式中文；画布先销毁时高亮由场景释放。
- 验证：26 项回归 25 绿，主窗口回归定位到刷新重声明引发的保存时图层销毁；修复后 tst_ui + tst_constraintediting 全绿。新用例包括实际拖点/加点/删点、边界多边形 round-trip、全字段 undo/redo、被拒撤销分支、三类捕捉、未保存放弃提示、缺失/只读资产、未绑定同步服务、画布场景销毁、活跃编辑中保存工程和批量改型下次单因素解析。
- 六维全量 diff 自审：store 与语义找到 Medium（刷新导致临时源被替换、非线改线语义、撤销表单陈旧）；资源找到 Medium（高亮场景先销毁）；i18n 找到 Medium（标准按钮未确保中文）；已逐项修复并补真实回归。分层/视觉 token 无偏离。此次仍非零发现轮，不计收口轮。
- 构建环境：GNU ld 2.47 并行链接两次 SIGSEGV，单目标串行重试可绿；最终工作树构建缓存改用已有 ld.lld（-fuse-ld=lld -Wl,--threads=1），每链接单线程、整体 -j8；不改仓库 toolchain、不装库、不动 vendor。
- 视觉：1600×1000 浅色/深色主窗口截图，含约束属性与可见 ribbon 设置；双主题须同时切换应用 palette 和壳 ribbon（遵循既有测试入口）。已人工核对控件边界、Noto/Mono、token 颜色、间距、虚线选择反馈。
- 下一步：最终全量构建与 26 项统一回归；随后连续两次全量 diff 六维复审。

## 轮3：参数保真复审

- 改动：删除旧属性保存面强制 enabled=true 的覆盖；UI 集成测试从禁用约束开始，验证批量改型与方向角/参数保存后 enabled=false、opaque 和 horizon 仍保留。
- 验证：修复前 26/26 全绿；修复后全量 -j8 构建绿，26 项重跑。旧 tst_constraintstore 文件零修改。
- 判定：六维复审发现 Medium（旧属性保存会改变未请求的启用参数），已修复。无分层、store 回滚、资源或 DESIGN 偏离；仍不计零发现轮。
- 下一步：只在最终测试完成后计连续零 High/Medium 的全量复审轮。

## 轮4：收口复审 A（从 store 到 UI）

- 改动：仅记录审查结果，代码冻结。
- 验证：最终全量构建绿；26/26 CTest 绿（build/editing-tests-final.log，43.64s）；17 个新编辑场景 + 主窗口真实入口回归；git diff --check 绿；原 tst_constraintstore 零修改。
- 全量范围：所有 tracked diff，以及新增 session 头/实现、shape preview 头、编辑测试和本 ledger，逐文件核对。

| 维度 | 结论与证据 |
|---|---|
| 分层 | 无 High/Medium；新 src 头标记齐全，qgis→io 依赖无循环；视图仅发属性意图，无新增越界 include；layering / strict / selftest 全绿。 |
| store 一致性 | 无 High/Medium；原生命令闭合与 undo/redo 事务写回；稳定 provider 避免删除计数/undo 重读偏差；失败恢复原生状态；整段取消独立于 undoLimit；FID/horizon/id 固定，参数空值与 double 精度保留。 |
| 语义 | 无 High/Medium；五语义沿既有 type 词表；未知参数/启用状态保留；方向角旋转真实几何；批量预校验拒绝混层位或非线要素；下次单因素解析 HardBarrier 用例绿。 |
| 资源 | 无 High/Medium；GDAL/OGR 句柄及时关闭，临时副本由会话持有；恢复正式源后清理；图层/画布销毁、捕获 deactivate/deleteLater、活跃编辑工程保存均有回归。 |
| DESIGN | 无 High/Medium；复用 native widget、ribbon、Noto Sans SC/JetBrains Mono 和现有 palette；双主题截图人工核对，未引入新颜色/字体/token。 |
| i18n | 无 High/Medium；新串走 tr + TS；退出按钮显式中文，两个 i18n 闸门全绿；TS XML 合法，改动 context 无重复 source。 |

- 判定：High=0、Medium=0；Low=1：每完成一条命令仍校验并事务重写整层位，进入会话复制 constraints 表；超大约束集合可后续按 FID 做差量写入/复制。无绝对墙钟性能断言。该 Low 写入 PR body。
- 下一步：从 UI 手势/错误路径反向复审全量 diff，要求第二次零 High/Medium。

## 轮5：反向复审发现会话重开守卫问题

- 改动：编辑服务持有当前约束会话的 destroyed 连接；成功保存/放弃后断开，避免前一段守卫在图层销毁时先销毁新会话、抢走新会话恢复 store 的回调。移除图层用例增加“关闭会话→重开→修改→销毁”路径。
- 验证：轮4 代码 26/26 全绿，但第二轮生命周期追踪发现重复守卫的顺序问题；新增重开路径与受影响回归需重跑。
- 判定：High=1（重开后图层移除可能错过初始快照恢复），已修复；先前零发现轮不能形成连续两轮。其它五维未新增 High/Medium；Low 的整层位重写仍保留。
- 下一步：全量构建/26 项验证；重新开始两轮零 High/Medium 的六维全量 diff 自审。
- 补充 URI 对账：活跃会话保存正式源时，按 QGIS provider metadata / project path resolver 序列化相对 URI，保留正常 .qgz 的迁移契约；回归把保存的 .qgz 和正式 GPKG 复制到新目录后直接重开，检查新路径有效。此修复也在重新开始零发现轮前完成。

## 轮6：最终收口复审 A（零 High/Medium，第 1 轮）

- 改动：仅补 ledger，代码冻结；新增文件纳入全量 diff。
- 验证：最终全量 -j8 构建绿；26/26 CTest 绿（76.17s，仅记运行证据，不作性能门）；17 个编辑场景含会话重开销毁和保存后移目录重开均绿；git diff --check 绿；旧 tst_constraintstore 无改动。
- 分层：头标记、qgis→io 链接、视图属性意图与编排边界逐文件复核，无 High/Medium，layering 三项绿。
- store 一致性：事务快照与原生栈同步、失败回退、完整取消、FID/身份/层位固定、精度/空值/未编辑字段均复核；稳定 provider 与重开守卫回归绿，无 High/Medium。
- 语义：五种类型词表、JSON 合并、geometryAzimuth 真几何旋转、非线/跨层位拒绝、opaque/enabled 保留、下次单因素读取均复核，无 High/Medium。
- 资源：OGR/GDAL、临时目录、当前会话销毁连接、scene 高亮、capture activate/deactivate 与延迟销毁逐路径复核；正式 URI 按原生 resolver 保存，无 High/Medium。
- DESIGN：双主题截图再次核对，原生控件/既有字体/间距/颜色和虚线选择反馈，无 High/Medium；ui_invariants 三项绿。
- i18n：新增 tr/TS、退出中文按钮、TS XML/重复 source 复核，无 High/Medium；i18n 两项绿。
- 判定：High=0、Medium=0、Low=1（整层位重写/复制成本，已记 PR body）。
- 下一步：反向复核同一份完整 diff；无代码改动才可形成连续第 2 轮。

## 轮7：最终收口复审 B（零 High/Medium，第 2 轮）

- 改动：仅记录复审/Oracle；轮6之后无功能代码改动。
- 验证：复用同一份最终全量构建与 26/26 回归结果；无需无因重复跑测。全量 diff 含全部新增文件再次检查，git diff --check 绿。
- 分层：从页面信号→窗口编排→工作流→QGIS→store 反向核对，QtWidgets/禁止 include/新增文件标记/CMake 最小测试链接无违规，High=0、Medium=0。
- store：分别追踪拖点/加点/删点、整要素删除、属性批量、undo/redo 分支、写入拒绝、栈满、完整取消、同层会话重开和移除；原生状态与正式 store 同步，High=0、Medium=0。
- 语义：逐字段追踪 type/params/horizon/FID、enabled=false、opaque、数值精度及方向真实几何；批量原子性与下次解析类型保持契约，High=0、Medium=0。
- 资源：从 ESC/deactivate/延迟销毁、canvas 先销毁、layer 销毁、service 结束、项目 XML 保存与移目录打开反向核对所有权和连接释放；工作副本不落入工程 URI，High=0、Medium=0。
- DESIGN：反向核对新增 ribbon spin、模式动作、属性控件和高亮；既有双主题 token/字体/间距保持，High=0、Medium=0。
- i18n：从可见标签/tooltip/错误/中文按钮回溯 tr 和 TS，测试壳配置独立沙箱；两个 i18n 闸门保持绿，High=0、Medium=0。
- 判定：最终连续轮6/轮7均 High=0、Medium=0。唯一 Low 为超大约束集合的整层位快照写/复制成本，PR body 已记录。

### Oracle 对账

| Oracle | 验证证据 |
|---|---|
| 顶点 round-trip，三件套保真 | tst_constraintediting::vertexRoundTripUndoRedoSave / vertexGesturesPersistInsertDeleteDrag / boundaryPolygonRoundTrip；geometry 与所有存储字段一致；tst_constraintstore 原文件未改且绿。 |
| 编辑→undo→redo，layer/store 逐字段一致 | Fixture::consistent 对 id/horizon/type/facies_code/weight/params_json/schema_version 和几何逐项比对；删除、分支失败、栈深截断、保存/取消路径全绿。 |
| 批量改型参与下次单因素 | batchSemanticUndoAndNextParser 直接调用 readConstraintLines，两条约束均为 HardBarrier；tst_ui 真实多选入口验证 opaque/enabled=false/horizon 保留、原生 undo/redo；非线批量拒绝不产生部分写入。 |
| 全量绿与纪律 | 全量构建 -j8；指定三套原验收 + 新编辑测试和其余受影响项合计26/26；layering×3 / ui_invariants×3 / i18n×2 / git diff --check 均绿；vendor 零修改；双主题截图人工核对。 |

- 下一步：按 store 会话 / UI 工具链 / ledger 三个原子提交，push 目标分支、创建 PR；不合并、不推 master、不等 CI。
