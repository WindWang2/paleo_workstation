# S5 独立评审：goal7/qgis-layers-layout-20261014

- 评审对象：`origin/master...baaa2efb`（5 提交 + R0 文档 + R7 微清理，26 文件）
- 评审口径：对照背景契约逐项核对行为守恒，独立怀疑性审查
- 验证手段：全量 diff 逐 hunk 阅读 + worktree 内增量构建 + 定向 ctest

## 验证记录（评审人实测）

| 检查 | 结果 |
|------|------|
| `cmake --build -j8`（tst_layoutexport / tst_layerplatform / tst_symbolpicker / tst_layertreepanel / tst_layerorganizer） | 通过（5/5 目标） |
| `ctest -R` 上述 5 个测试 | 5/5 Passed（layertreepanel 首次 BAD_COMMAND 为链接后瞬态 exec 竞态，重跑即过） |
| `ctest -R "^(layering\|layering_strict\|layering_selftest\|l10n\|l10n_selftest)$"` | 5/5 Passed |
| 变更文件层头注释（`// 层：...`） | 全部在位 |
| 变更文件 tr/translate 用法 | 符合契约 |

## 行为守恒逐项核对（全部通过）

1. **QgisLayerOrganizer 哈希化**（qgislayerorganizer.cpp:146-180, 342-385）：
   `declaredById`/`declaredIds` 快照替换逐节点复读 sqlite，语义等价性逐一验证——
   同 layerId 多声明取首个 == 旧线性扫描 `break`；`declaredIds.contains(paleoId)` 对
   空 paleoId 恒 false == 旧 `!pid.isEmpty()` 守卫；clone+insert+remove 归位路径未触碰；
   用户拖拽不被接管的逻辑未触碰。快照陈旧窗口不存在：声明表是 manifest 数据，
   遍历中途的节点搬动不写声明表。`placeLayer(layer, decls=nullptr)` 默认现场取表，
   单事件路径与旧实现逐字节同语义。
2. **refreshIndicators**（layertreepanel.cpp:706-748）：`horizonById` 首声明胜出 +
   `!it.value().isEmpty()` 条件与旧 `break` 扫描等价；无效层警告 indicator 分支未动；
   indicator 父子关系不变（parent = m_view，旧代码同）。无新增信号。
3. **applyPrunedTheme**（qgislayerprofile.cpp:312-368）：守恒仅对 `page:*` 主题启用，
   用户命名主题完全不走快照/恢复路径（`isPageTheme` 门）。恢复按 QGIS layer id
   回查节点，快照在 `collection->update`/`applyTheme` 之前、两者均不删节点不搬父，
   `findLayer` 不失效；恢复后 `ensureSharedDataVisible()` 顺序正确（声明层不受
   守恒影响，共享层置顶裁定不被回滚）。祖先链只向 true 恢复、自身勾选按快照
   双向还原——已推演无「勾回祖先带出声明层/其他未声明层」的漏显路径
   （声明层自身勾选仍由主题记录持有）。`setItemVisibilityChecked` 的
   visibilityChanged 无回路重入 applyPrunedTheme（全仓 grep 无此类连接）。
4. **layouttemplates 失败收口**（layouttemplates.cpp:118-131 + 全 fail 迁移）：
   所有旧 `templateFinished(path,false)` 失败点统一走 `fail()` = 旧信号 + 新增
   `templateFailed` 带全文 + ErrorHub warning；`ErrorHub::global()==nullptr` 时不弹窗
   （契约要求）。成功路径信号序列不变（含 save/load UI lambda 的成功 emit 未动）。
   ErrorHub 60s 去重聚合是其既有契约，非本分支引入。
5. **mapbooklayout validateExportedFile**（mapbooklayout.cpp:358-416）：
   PDF = %PDF 头 + 末 1KiB `%%EOF`；PNG = 8 字节签名 + 可解码 + 像素 ±2px
   （期望 = 首页 mm × dpi/25.4，非 mm 单位页跳过）；SVG = 头 4KiB 含 `<svg`。
   新增 SVG 校验是**补零校验**（旧代码 SVG 直接判成功），属契约内加固。
   `labelFormat` 的唯一调用方原本传硬编码字面量 "Noto Sans CJK SC"（mapbooklayout.cpp:58），
   非用户可配，Q_UNUSED 无行为漂移；指北针工厂搜索列表/顺序与旧内联代码逐字等价
   （standardelements.cpp:168-183）。
6. **翻译上下文切换**（geopatterns.cpp / qgisstyleservice.cpp）：
   中文源串未动；ts 已在 master 预登记 `GeoPatterns` 上下文（QT_TRANSLATE_NOOP 一直
   用 "GeoPatterns"，运行时用 QObject::tr 查 "QObject" 才是本分支修的 bug）；
   `QgisStyleService` 上下文已在 ts 中就位。逐条比对：本次迁移的 23 个词面在
   QObject 上下文中**全部为恒等译文**，无用户可见回归。

## 发现

### Blocker

无 blocker。

### Major

无 major。

### Minor

1. **qgislayerprofile.cpp:320-323 — 注释与代码自相矛盾（失败路径语义被误述）。**
   注释称「读失败时……带 id 但查证失败的层不动豁免，交给主题记录照常裁定」，
   但代码实际是：`tryDeclared` 失败 → `declaredIds` 为空 → skip 条件
   `!paleoId.isEmpty() && declaredIds.contains(paleoId)` 对**所有**带 paleoId 的层
   为 false → 声明层也进守恒集，主题记录完全不裁定。代码行为本身是安全的保守
   退化（宁可不动不可错藏），但与紧邻注释、与 qgislayerprofile.h 头注释的表述
   不符，后续维护者会按错误注释改代码。建议：改注释如实描述「读失败 → 全部
   层守恒、主题重放空转」；或者若产品上希望失败时仍由主题裁定，把条件改成
   仅当 tryDeclared 成功时才建守恒集。
2. **m_layerService 为 nullptr 时 page:* 重放整体空转（qgislayerprofile.cpp:315）。**
   `m_layerService` 默认 nullptr（qgislayerprofile.h:122），此时所有层都进守恒集，
   `applyTheme` 的可见性裁定被完全架空。生产路径几乎总注入 service，属防御性
   退化；但与修复前「重放会真实改可见性」相比仍是可观察的语义漂移，建议注释
   或日志留痕（一条 qWarning 足够）。
3. **ts 文件未随上下文切换刷新（translations/paleo_zh_CN.ts）。**
   迁移后的词面在 QObject 上下文的旧条目将成为 lupdate 下的 vanished 死串
   （check_l10n 仅 warn 级，不挡 CI；当前译文全恒等，无可见影响）。但这是
   「lupdate 收口」纪律（方向 100）意义上的账，建议下个 lupdate 周期清理，
   并在词表别名类改动（kFaciesAliases）后顺手跑一次。
4. **tst_layoutexport.cpp:700 mapbookLabelsUseFigureFontFactory 探针有一半同义反复。**
   `expected` 取自 `PaleoStandardElements::figureTextFormat(9.0)` 本身——
   它钉住的是「标签走了工厂」这一接线事实（修复前确实红），但钉不住
   「工厂解析到 vendored 族」：若 figureTextFormat 自身退化成默认族，
   标签与 expected 同步退化，用例照样绿。注释声称 initTestCase 钉了渲染
   环境，但本用例没有直接断言 family 非空/等于 "Noto Sans SC"。建议补一行
   `QVERIFY(!expected.isEmpty())`（或对照 vendored 族名），让探针具备对
   工厂退化的敏感度。

### Nit

5. **mapbooklayout.cpp:358-365 — validateExportedFile 中 QFile 未显式 close**
   即在 PNG 分支再构造 `QImage(path)`。POSIX 无害，Windows 下依赖两次只读
   共享打开不冲突；函数返回时 RAII 关闭。建议在 QImage 前加 `file.close()`
   一行，消除平台心智负担。
6. **qgislayerorganizer.cpp:153-156 — `owned` 空向量占位写法可读性**
   `decls ? QVector<LayerDeclaration>() : m_layerSvc->declared()` 之后
   `const &all = decls ? *decls : owned`——功能正确（空 QVector 不分配堆），
   但双三元绕一层。可接受，不改也行。
7. **探针测试的墙钟宽门**（tst_layerorganizer.cpp:552/570 30s、
   tst_layertreepanel.cpp:900/905 30s/10s）：作者已自注「宽门只挡灾难性回归 +
   qInfo 落盘看量级」，口径诚实；重度负载 CI 上仍有理论抖动面，但门宽到
   只有数量级劣化才会红，风险可接受。
8. **previewmapcanvas.h:163 删友元**：全仓 grep 确认无 `PreviewMapCanvasSelftest`
   残留引用，死代码清除干净，无遗漏。

## 测试质量总评

新增 8 个用例中 6 个是高质量钉扎：W1 页主题重放用例覆盖三种后加临时层拓扑
（树根/既有组/新建组）+ 用户手动取消勾选的「不得替用户勾回」+ 声明层仍被
主题裁定的双向守恒，修复前必红、修后语义完备；W4 探针翻译器用例对修复前代码
确红（QObject 上下文查不中）；W5 同时断言信号载荷、hub 入账与成功路径零污染；
W7 三个负向注入（截断 PDF/错尺寸 PNG/假 SVG）+ 三个真实导出正向通过，防
误伤。两个 perf 探针（W2/W3）附正确性守恒断言，非纯计时。扣分项仅 minor 4
的同义反复半面。

## 总体结论

**PASS-with-minors。** 无 blocker、无 major。五条关键契约逐 hunk 核对全部守恒，
测试不是走过场（多用途例验证了对修复前代码确红），layering/l10n 门禁绿。
建议合入前处理 minor 1（注释与代码矛盾的误导性最高），minor 2/3/4 可随后续
迭代带上。
