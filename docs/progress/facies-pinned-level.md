# 编图相类固定显示层级列表

2026-10-24。

在三级尺度显示（自动比例尺 / 图层固定层级）与单面显示锁定的基础上，
「相类别与标注」新增「相类固定层级」列表：选择某个相、亚相或微相节点，
指定「固定显示为」相 / 亚相 / 微相，点击「固定」后该节点及其整棵子树
始终按指定层级显示，不随比例尺或图层显示层级变化。列表中每行可直接改选
层级，「移除固定」恢复跟随图层。分类中已不存在的固定项标注
「（不在当前分类中）」，仍可移除。

优先级：单面锁定 `facies_display_level` > 相类固定（最近的固定祖先优先，
叶节点固定覆盖父节点固定）> 图层显示层级 / 自动比例尺。

固定项保存在图层自定义属性 `paleo/faciesPinnedLevels`（JSON：节点键 → 层级），
随 .qgz 工程保存，与 `paleo/faciesDisplayMode` 同一机制；不改 GeoPackage
结构，无迁移，旧工程没有该属性即视为无固定，显示不变。非法键或层级在
读取时丢弃。井预测图层同样可固定（固定项覆盖其默认微相显示）。

渲染器在显示层级表达式中按微相编码插入固定层级分支，图例按实际出现的
编码分别归入有效层级，画布、图层树、版面和导出共用同一渲染器。

回归用例：`tst_faciescatalog`（最近祖先解析、非法项丢弃、编码往返）与
`tst_mappingworkbench`（跨比例尺与图层固定模式下的子树固定、单面锁定优先、
改级 / 非法请求 / 移除、工程保存重开持久化与旧工程无固定、列表增删改意图）。

后续补齐（PR #691 Low 项）：

- 编辑副本与新保存版本：固定项写入版本记录 extra 的 `pinned_levels`
  作为种子；新图层尚无属性时由 `styleLayer` 带入，已有属性（含显式清空
  写入的 `"{}"`）以图层为准。存储仍是图层属性，GeoPackage 无改动。
- Ribbon 显示层级控件：下拉选项与行为不变，有固定时状态文字与提示附加
  「N 个相类固定」。
- 参考窗口：以主图图层当前固定为准传入，与主图同口径显示。
- 编码匹配：按字段原值类型生成带类型字面量（`QgsExpression::quotedValue`），
  不再经 `to_string`，整型与文本编码字段均可匹配。

新增用例：`classPinsCarryIntoSavedVersionsAndEditingCopies`、
`classPinLiteralsKeepFieldTypes`、`referenceCanvasHonoursClassPins`、
`ribbonDisplayControlIndicatesPinnedClasses`，以及
`wellIntervalsReviewUndoAndPersistence` 中的测井固定段。
