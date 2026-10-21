# 索引式数据管理与连井剖面画线吸附 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按规格 `docs/superpowers/specs/2026-10-21-indexed-data-mgmt-and-section-snapping-design.md` 落地三件事：导入默认只建索引 + 数据对象可命名；连井剖面画线吸附井位 + 吸附即井序 + 空间重排；删除数据对象索引（文件一律不动）+ 孤儿受管文件治理扫描。

**Architecture:** 数据层（`src/catalog` 加 `renameAsset`；`src/io` 加 `ImportOptions.displayName` / `ImportSession.linkExternal` / `PlannedItem.displayName`）只问答；功能层（`src/workflow` 的 `FolderImportWorkflow` 持 UI 默认翻转、`SectionWorkbench` 出空间排序与吸附井集、`paleo::assetops` 出 keepFiles 删除与孤儿扫描）只编排；视图层（`datalist_*` 菜单/命令、`SectionSetupDialog`、壳 `paleomainwindow_sections`）只发信号与装配；`src/domain/wellsection` 出纯算法（最近邻链 / 最近井查找，可单测）；`src/qgis/SeismicSectionTool` 做画布交互（QGIS 封装豁免层）。

**Tech Stack:** C++17 / Qt 6.11.2 / QGIS 4.2 / CMake + Ninja / ctest。Windows 本机构建走 `paleo-dev.ps1` localdeps 路线（BUILDING.md「Windows 本机开发」「独立 worktree 开发」两节）。

**通用纪律（每个任务都适用）：**
- 新 `src/` 文件头三行必须 `// 层：<数据|功能|QGIS 封装|视图>`；层禁令见 AGENTS.md。
- 构建 `cmake --build build -j8 --target <t>`；测试 `ctest --test-dir build -R '^<name>$' --output-on-failure`（经 `paleo-dev.ps1 test -- <args>` 亦可，它会处理 TEMP/TMP 沙箱重定向）。并行度一律 ≤ `-j8`。
- 构建前先 `powershell -File paleo-dev.ps1 checkenv` 确认 Qt 链不混。
- TDD：先写失败测试 → 跑红 → 最小实现 → 跑绿 → 提交。提交信息中文 conventional 风格（参照 git log）。
- 界面文案用 `tr()`；i18n 闸 `ctest -R i18n`。
- 每完成一个任务跑一次 `python tools/check_layering.py` 相关闸门中最快的一个（`ctest --test-dir build -R '^layering$'`），确保不新增违规。
- 测试函数体保持小（file_budget 闸按函数体规模算）。

---

## Task 0: worktree 与构建接线确认

**Files:** 无（环境）

- [ ] **Step 1: 确认基线并建 worktree**

```bash
cd C:/Users/wangj.KEVIN/projects/paleo_workstation
git fetch origin
git rev-parse master origin/master   # 两者一致则以 master 为基线；不一致先问用户
git worktree add ../paleo_workstation-indexed-mgmt -b codex/indexed-data-mgmt-section-snapping master
```

- [ ] **Step 2: 把规格与本计划文件带入 worktree 并提交**

```bash
cd ../paleo_workstation-indexed-mgmt
mkdir -p docs/superpowers/specs docs/superpowers/plans
cp ../paleo_workstation/docs/superpowers/specs/2026-10-21-indexed-data-mgmt-and-section-snapping-design.md docs/superpowers/specs/
cp ../paleo_workstation/docs/superpowers/plans/2026-10-21-indexed-data-mgmt-and-section-snapping.md docs/superpowers/plans/
git add docs/superpowers && git commit -m "docs: 索引式数据管理与连井剖面画线吸附——规格与实现计划"
```

- [ ] **Step 3: 配置构建（localdeps，参 BUILDING.md Windows 节）**

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=$HOME/paleo-qgis-deps/Library" `
  "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=C:/deps/Qt/6.11.2/msvc2022_64" `
  "-DQGIS_PREFIX=$HOME/paleo-qgis-prefix" `
  "-DQSCINTILLA_PREFIX=$HOME/paleo-qgis-deps/Library/include/qt6" `
  "-DPALEO_TEST_PYTHON=$HOME/paleo-qgis-deps/python.exe"
cmake --build build -j8
```

预期：configure 成功（若漏 glm/saribbon/sbm/segyio include，重跑同一条 configure——已知接线问题）；全量构建绿。后续任务一律在此 worktree 内工作。

---

## 阶段一（功能一）：索引式导入 + 数据对象命名

### Task 1: `DataCatalog::renameAsset` 数据层 API

**Files:**
- Modify: `src/catalog/datacatalog.h`（声明放 `updateLink` 之后，约 :231 处）
- Modify: `src/catalog/datacatalog_assets.cpp`
- Test: `tests/tst_catalog.cpp`

- [ ] **Step 1: 写失败测试**

`tests/tst_catalog.cpp` 新增用例（沿用该文件既有临时工程 fixture 模式）：

```cpp
void TestCatalog::renameAssetPersists()
{
  const QString dir = makeTempProject();           // 沿用文件内既有助手
  DataCatalog cat;
  QVERIFY(cat.open(dir));
  CatalogAsset a; a.id = cat.nextAssetId(); a.type = "well_log";
  a.format = "las"; a.displayName = "A1_log.las";
  QVERIFY(cat.addAsset(a));
  // 重命名成功并立即可读
  QVERIFY(cat.renameAsset(a.id, "A1 主测井"));
  QCOMPARE(cat.assetById(a.id).displayName, QString("A1 主测井"));
  // 同名调用 = 无操作成功（不涨 revision）
  const int rev = cat.catalogRevision();
  QVERIFY(cat.renameAsset(a.id, "A1 主测井"));
  QCOMPARE(cat.catalogRevision(), rev);
  // 空名/纯空白拒绝
  QVERIFY(!cat.renameAsset(a.id, "   "));
  // 未知 id 拒绝
  QVERIFY(!cat.renameAsset("ast-9999", "x"));
  cat.close(); // 若无 close() 用既有销毁-重开模式
  // 重开工程持久化
  DataCatalog cat2;
  QVERIFY(cat2.open(dir));
  QCOMPARE(cat2.assetById(a.id).displayName, QString("A1 主测井"));
}
```

（若 `tst_catalog.cpp` 无 `close()` 先例，改为作用域析构后重开；测试函数名按该文件既有命名风格调整。）

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_catalog && ctest --test-dir build -R '^tst_catalog$' --output-on-failure`
预期：编译失败 `'renameAsset' is not a member of 'DataCatalog'`。

- [ ] **Step 3: 实现**

`datacatalog.h`（`updateLink` 声明后）：

```cpp
    // 重命名数据资产（方向：数据对象自定义名称）：就地更新 assets.displayName。
    // 物理文件名（CatalogVersion::fileName）不动。trim 后为空 → false+error；
    // 同名 → true 无落盘（不空涨 revision，removeStaleVersions 同口径）。
    // staging 副本拒绝（journal 无此 op——removeAsset 同一先例）。
    bool renameAsset(const QString &assetId, const QString &newName, QString *error = nullptr);
```

`datacatalog_assets.cpp`（`removeAssets` 实现之后）：

```cpp
bool DataCatalog::renameAsset(const QString &assetId, const QString &newName, QString *error)
{
  if (!checkWriteThread("renameAsset", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (m_staging)
  {
    setError(error, QStringLiteral("staging copy does not support renameAsset (journal has no rename op)"));
    return false;
  }
  const QString trimmed = newName.trimmed();
  if (trimmed.isEmpty())
  {
    setError(error, QStringLiteral("asset display name must not be empty"));
    return false;
  }
  const int row = m_idx.assetRow(assetId);
  if (row < 0)
  {
    setError(error, QStringLiteral("asset not found: %1").arg(assetId));
    return false;
  }
  if (m_assets.at(row).displayName == trimmed)
    return true; // 无操作成功：不落盘不涨 revision
  const QString prev = m_assets.at(row).displayName;
  m_assets[row].displayName = trimmed;
  m_dirtyAssets.insert(assetId);
  if (save(error))
  {
    ++m_mutationSeq; // 同 removeAssets 口径：成功变更推进基线序号（无 journal op）
    return true;
  }
  m_assets[row].displayName = prev;
  return false;
}
```

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_catalog && ctest --test-dir build -R '^tst_catalog$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/catalog tests/tst_catalog.cpp && git commit -m "feat(catalog): 数据资产重命名 API renameAsset——就地更新 displayName，物理文件名不动"`

---

### Task 2: 数据对象重命名 UI（命令 + 菜单 + F2）

**Files:**
- Modify: `src/ui/pages/dataops/dataopscommands.h`（文件尾 `} // namespace` 前加 `RenameAssetCmd`）
- Modify: `src/ui/pages/dataopspanelops.h:419-465`（`contextMenuActions` 矩阵）
- Modify: `src/ui/pages/datalist.h`（声明 `renameSelectedAsset()`）
- Create: `src/ui/pages/datalist_indexops.cpp`（新处理器；datalist 部分类拆分先例，规避 file_budget）
- Modify: `src/ui/pages/datalist_batch.cpp:520-563`（kTitles 加一行）与 `:631` 附近（dispatch 加分支）
- Modify: `src/ui/pages/datalist.cpp:930-936`（F2 扩展到资产）
- Test: `tests/tst_panels_dataops.cpp`（菜单矩阵断言更新 + 命令用例）

- [ ] **Step 1: 写失败测试（矩阵 + 命令）**

矩阵用例（`tst_panels_dataops.cpp:136-192` 区域，更新既有期望列表——单资产期望含 `"renameAsset"`，位置紧随 `"openPreview"` 之后；多资产期望列表也含 `"renameAsset"`）：

```cpp
// 单资产
QCOMPARE(acts, QStringList({"openPreview", "renameAsset", "showVersions", ...}));
```

命令用例（新）：

```cpp
void TestPanelsDataops::renameAssetCmdUndoRedo()
{
  // 沿用文件内既有 DataOpsContext fixture（含临时工程 catalog）
  const QString id = addTestAsset(ctx, "old.las"); // 若文件内已有类似助手直接用；否则内联 addAsset
  paleo::dataops::RenameAssetCmd cmd(ctx, id, "old.las", "新名字");
  cmd.redo();
  QCOMPARE(ctx.cat->assetById(id).displayName, QString("新名字"));
  cmd.undo();
  QCOMPARE(ctx.cat->assetById(id).displayName, QString("old.las"));
}
```

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_panels_dataops && ctest --test-dir build -R '^tst_panels_dataops$' --output-on-failure`
预期：编译失败（`RenameAssetCmd` 不存在）或矩阵断言失败。

- [ ] **Step 3: 实现**

`dataopscommands.h` 尾部（namespace 结束前）：

```cpp
// ---- 数据资产重命名（方向：数据对象自定义名称；catalog renameAsset 写路径）----
class RenameAssetCmd : public DataOpCommand
{
public:
  RenameAssetCmd(DataOpsContext ctx, QString assetId, QString prevName, QString nextName)
    : m_ctx(ctx), m_assetId(std::move(assetId))
    , m_prev(std::move(prevName)), m_next(std::move(nextName))
  {
  }
  void redo() override
  {
    if (!m_ctx.valid())
      return;
    m_ctx.cat->renameAsset(m_assetId, m_next);
  }
  void undo() override
  {
    if (!m_ctx.valid())
      return;
    m_ctx.cat->renameAsset(m_assetId, m_prev);
  }
  QString text() const override
  {
    return QObject::tr("重命名 %1 → %2").arg(m_prev, m_next);
  }
  QByteArray id() const override { return QByteArray(); } // 不并栈

private:
  DataOpsContext m_ctx;
  QString m_assetId, m_prev, m_next;
};
```

`dataopspanelops.h` `contextMenuActions`：在 `acts << QStringLiteral("openPreview");` 行后插入：

```cpp
    if (spec.assetCount == 1 && !spec.hasEntities)
      acts << QStringLiteral("renameAsset"); // 数据对象自定义名称（单选资产）
```

`datalist.h`（`removeAssetsSoft` 声明附近）加：

```cpp
    void renameSelectedAsset();        // datalist_indexops.cpp：重命名单选数据对象
    void removeFromCatalogKeepFiles(); // datalist_indexops.cpp：删索引保文件（Task 12 实现）
```

新建 `src/ui/pages/datalist_indexops.cpp`：

```cpp
// 层：视图
// DataListPanel 部分类（datalist_* 拆分先例）：数据对象重命名与索引级删除的
// 处理器。命令走共享 undo 栈；索引级删除不可撤销（确认文案明示）。
#include "datalist.h"
#include "dataops/dataopscommands.h"
#include "paleonotify.h"
#include "../../workflow/assetops.h"
#include <QInputDialog>

void DataListPanel::renameSelectedAsset()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.size() != 1 || !m_ctx.valid() || m_ctx.cat->refusesWrites())
    return;
  const QString id = *ids.constBegin();
  const QString prev = m_ctx.cat->assetById(id).displayName;
  if (prev.isEmpty())
    return;
  bool ok = false;
  const QString next = QInputDialog::getText(this, tr("重命名数据对象"),
                                             tr("新名称（物理文件名不变）："),
                                             QLineEdit::Normal, prev, &ok).trimmed();
  if (!ok || next.isEmpty() || next == prev)
    return;
  pushCommand(new RenameAssetCmd(m_ctx, id, prev, next));
  if (m_history)
    m_history->push(tr("重命名 %1 → %2").arg(prev, next));
  emit statusMessage(tr("已重命名为「%1」（可撤销）").arg(next));
}
```

`datalist_batch.cpp` kTitles 表（`:524-549`）`{"openPreview", ...}` 行后加：

```cpp
    {"renameAsset", QT_TR_NOOP("重命名数据对象…")},
```

dispatch（`:572` 区域）加分支：

```cpp
  else if (key == QLatin1String("renameAsset"))
    renameSelectedAsset();
```

`datalist.cpp` F2 shortcut（`:930-936`）改为：

```cpp
  addShortcut("data.list.rename", "scRename", [this] {
    // 树内实体节点 F2 → 实体重命名意图（EntityPanel/壳接）；
    // 单选数据资产 F2 → 数据对象重命名。
    const QStringList ents = currentEntitySelection();
    if (!ents.isEmpty())
    {
      emit entityRenameRequested(ents.front());
      return;
    }
    if (currentAssetSelection().size() == 1)
      renameSelectedAsset();
  });
```

CMake：`datalist_indexops.cpp` 加进 ui 库源列表（找到 `datalist_batch.cpp` 所在源清单同处添加；先 `grep -rn "datalist_batch.cpp" CMakeLists.txt cmake/ src/` 定位）。

- [ ] **Step 4: 跑绿 + 闸门**

`cmake --build build -j8 --target tst_panels_dataops && ctest --test-dir build -R '^tst_panels_dataops$' --output-on-failure && ctest --test-dir build -R '^layering' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/ui tests/tst_panels_dataops.cpp CMakeLists.txt cmake && git commit -m "feat(dataops): 数据对象重命名——右键/F2 入口 + RenameAssetCmd 可撤销，写路径走 catalog renameAsset"`

---

### Task 3: 导入时命名 + 单文件向导存储勾选

**Files:**
- Modify: `src/io/dataimportservice.h:36-42`（`ImportOptions` 加 `displayName`）
- Modify: `src/io/dataimport_file.cpp:313`（`asset.displayName` 赋值口径）
- Modify: `src/workflow/folderimport.h:89-90`（`importFile` 加尾参）
- Modify: `src/workflow/folderimport.cpp:552-594`（两分支传参）
- Modify: `src/ui/paleomainwindow_attach_data.cpp:405-438`（向导第 2 页加名称框与复制勾选）
- Test: `tests/tst_import.cpp`

- [ ] **Step 1: 写失败测试**

`tests/tst_import.cpp`（沿用该文件临时工程 + DataImportService fixture）：

```cpp
void TestImport::importDisplayNameOptionHonored()
{
  const QString src = writeTestLas("A9_log.las"); // 沿用文件内既有造文件助手
  DataImportService::ImportOptions opts;
  opts.displayName = QStringLiteral("A9 主测井曲线");
  QString err;
  const auto res = svc->importProjectFileEx(src, opts, &err);
  QVERIFY2(res.outcome != DataImportService::ImportOutcome::Failed, qPrintable(err));
  QCOMPARE(svc->catalog()->assetById(res.assetId).displayName, QString("A9 主测井曲线"));
  // 版本物理文件名保持原文件名
  QCOMPARE(svc->catalog()->currentVersion(res.assetId).fileName, QString("A9_log.las"));
  // 空 displayName → 回退原文件名
  const auto res2 = svc->importProjectFileEx(writeTestLas("B2_log.las"), {}, &err);
  QCOMPARE(svc->catalog()->assetById(res2.assetId).displayName, QString("B2_log.las"));
}
```

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：编译失败（`ImportOptions` 无 `displayName` 成员）。

- [ ] **Step 3: 实现**

`dataimportservice.h` `ImportOptions`（`:36-42`）加一行：

```cpp
      QString displayName;       // 非空 → 资产显示名（默认原文件 basename；物理 fileName 恒不动）
```

`dataimport_file.cpp:313` 改为：

```cpp
  asset.displayName = options.displayName.trimmed().isEmpty() ? fi.fileName()
                                                              : options.displayName.trimmed();
```

`folderimport.h` `importFile` 签名改为：

```cpp
    // 单文件导入（数据页「导入」按钮）：同 importFolder 的双路径语义。
    // linkExternal 默认 true（方向：UI 默认只建索引）；displayName 非空 →
    // 覆盖资产显示名（导入时命名）。
    void importFile(const QString &kind, const QString &path,
                    std::function<void(const QString &assetId, const QString &error)> done,
                    bool linkExternal = true, const QString &displayName = {});
```

`folderimport.cpp:566-593` 两处构造 `ImportOptions` 后各加（捕获列表补 `linkExternal, displayName`）：

```cpp
      options.linkExternal = linkExternal;
      options.displayName = displayName;
```

`paleomainwindow_attach_data.cpp` 向导 review 页（`:405-419` 区域，`crsNote` 之后）加：

```cpp
              auto *nameLabel = new QLabel(tr("数据对象名称："), reviewPage);
              reviewLayout->addWidget(nameLabel);
              auto *nameEdit = new QLineEdit(QFileInfo(path).fileName(), reviewPage);
              nameEdit->setObjectName(QStringLiteral("singleImportNameEdit"));
              reviewLayout->addWidget(nameEdit);
              auto *copyCheck = new QCheckBox(tr("将文件复制进工程目录（受管副本）"), reviewPage);
              copyCheck->setObjectName(QStringLiteral("singleImportCopyCheck"));
              copyCheck->setToolTip(tr("默认只建立索引引用原文件，不复制；勾选后复制进工程 artifacts/raw"));
              reviewLayout->addWidget(copyCheck);
```

调用处（`:431-438`）改为：

```cpp
              if (auto *wf = folderImportWorkflow())
                wf->importFile(effectiveType, path, [](const QString &assetId,
                                              const QString &err) {
                  if (assetId.isEmpty())
                    QgsMessageLog::logMessage(
                        QObject::tr("导入失败：%1").arg(err),
                        QStringLiteral("Paleo"), Qgis::Critical);
                }, !copyCheck->isChecked(), nameEdit->text());
```

（`QCheckBox`/`QLineEdit` include 按文件头现存情况补。）

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/io src/workflow src/ui/paleomainwindow_attach_data.cpp tests/tst_import.cpp && git commit -m "feat(io): 导入时命名——ImportOptions.displayName 贯通，单文件向导加名称框与「复制进工程」勾选"`

---

### Task 4: UI 导入默认只建索引（`ImportSession.linkExternal` 贯通）

**Files:**
- Modify: `src/io/dataimportservice.h`（`ImportSession` 输入区，`:431 wellTargets` 附近加字段）
- Modify: `src/io/dataimport_folder.cpp:608-613`（`opts.linkExternal = s.linkExternal;`）
- Modify: `src/workflow/folderimport.h`（成员 + setter）
- Modify: `src/workflow/folderimport.cpp`（所有给 session 配置 `wellTargets` 的点同步配置 `linkExternal`：至少 `:382` 与 `:468` 两处；`grep -n "wellTargets =" src/workflow/folderimport.cpp` 找全）
- Modify: `src/ui/dialogs/folderconfirm.h`（确认表加「复制进工程」勾选 + `copyIntoProject()` 访问器）
- Modify: 确认表调用点（`src/ui/paleomainwindow_project.cpp:58-175` 多文件链；文件夹导入 `runFolderImport` 所在文件）：接受后 `wf->setLinkExternal(!dlg.copyIntoProject())`
- Test: `tests/tst_import.cpp`（io 层贯通）+ FolderImportWorkflow 测试（先 `grep -rln "FolderImportWorkflow" tests/` 定位；无则在 `tst_import.cpp` 末尾加，伞式链接足够）

- [ ] **Step 1: 写失败测试**

io 层（`tst_import.cpp`）：

```cpp
void TestImport::sessionLinkExternalProducesUnmanagedVersion()
{
  auto session = svc->beginImport();
  QVERIFY(session);
  session->linkExternal = true;
  const QString src = writeTestLas("C3_log.las");
  DataImportService::produceFiles(*session, {src}, {}, {});
  QString err;
  QCOMPARE(svc->commitImport(*session, &err), DataImportService::CommitStatus::Committed);
  const CatalogVersion v = svc->catalog()->currentVersion(
      svc->catalog()->assets().last().id);
  QVERIFY(!v.managed);                       // 只建索引
  QCOMPARE(v.path, QDir(src).absolutePath()); // catalog 指向原文件
  QVERIFY(!v.sha256.isEmpty());
  QVERIFY(QFile::exists(dir->filePath("artifacts/raw")) == false || ...); // 工程内无该副本
}
```

（最后一条断言按 fixture 便利程度简化为「`artifacts/raw` 下不存在以该资产 id 命名的目录」。）

workflow 层：

```cpp
void TestImport::folderImportWorkflowDefaultsToLinkExternal()
{
  FolderImportWorkflow wf(svc, nullptr);   // 无任务服务 → 同步路径
  // 不设 setLinkExternal：默认即 true
  const QString src = writeTestLas("D4_log.las");
  bool doneFlag = false;
  wf.importFiles({src}, {}, {}, [&](const QVector<FolderRowResult> &rows, const QString &err) {
    QVERIFY2(err.isEmpty(), qPrintable(err));
    doneFlag = true;
  });
  QVERIFY(doneFlag);
  QVERIFY(!svc->catalog()->currentVersion(svc->catalog()->assets().last().id).managed);
}
```

（签名细节按 `importFiles` 实际参数序调整；若该测试放别处，保持同一断言意图。）

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：编译失败（`ImportSession` 无 `linkExternal`）。

- [ ] **Step 3: 实现**

`dataimportservice.h` `ImportSession`（`wellTargets` 声明旁）：

```cpp
  bool linkExternal = false; // UI 默认只建索引（方向 D1）：produce 期输入；io 层默认保持受管语义
```

`dataimport_folder.cpp:609` 区域：

```cpp
  opts.linkExternal = s.linkExternal;
```

`folderimport.h`（`setTaskService` 旁）：

```cpp
    // UI 导入默认只建索引（方向 D1）：整批/文件夹/拖入链在 session 配置点
    // 统一注入；false = 恢复旧受管复制默认（确认表「复制进工程」勾选）。
    void setLinkExternal(bool on) { m_linkExternal = on; }
    bool linkExternal() const { return m_linkExternal; }
```

private 成员区加 `bool m_linkExternal = true;`。

`folderimport.cpp`：所有 `session->wellTargets = ...;` / `s.wellTargets = ...;` 配置点下一行各加：

```cpp
  session->linkExternal = m_linkExternal;   // :382 同步路径
  s.linkExternal = m_linkExternal;          // :468 任务池路径（捕获无需变——m_linkExternal 经 this 读）
```

确认表 `folderconfirm.h`：加 `QCheckBox *m_copyCheck`（objectName `folderImportCopyCheck`，文案 `将文件复制进工程目录（受管副本）`，默认不勾选，放确认按钮区上方）与：

```cpp
  bool copyIntoProject() const { return m_copyCheck && m_copyCheck->isChecked(); }
```

调用点（`paleomainwindow_project.cpp` 多文件确认与 `runFolderImport`）：dialog `exec()` 接受之后、调 `importFolder/importFiles` 之前：

```cpp
  if (auto *wf = folderImportWorkflow())
    wf->setLinkExternal(!dlg.copyIntoProject());
```

（拖入链 `externalImportRequested` 若不经确认表，则走成员默认 true——即只建索引，符合 D1。）

- [ ] **Step 4: 跑绿 + 既有导入回归**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：PASS。注意：`tst_import.cpp` 中钉「默认受管」的用例是 io 层（直调 `importProjectFile`/默认 session），不受影响；若有经 `FolderImportWorkflow` 的既有用例期望受管，按 D1 新口径改为显式 `wf.setLinkExternal(false)`。

- [ ] **Step 5: 提交**

`git add src/io src/workflow src/ui tests/tst_import.cpp && git commit -m "feat(io): UI 导入默认只建索引——ImportSession.linkExternal 贯通批量/文件夹/拖入链，受管复制降级为确认表勾选项"`

---

### Task 5: 批量确认表显示名列（nameOverrides 贯通）

**Files:**
- Modify: `src/io/ingestplan.h:31-47`（`PlannedItem` 加 `displayName`）
- Modify: `src/io/dataimport_folder.cpp`（`produceItem` 应用；`produceFromPlan`/`produceFolder`/`produceFiles` 加 `nameOverrides` 形参并并入 plan items）
- Modify: `src/io/dataimportservice.h`（上述静态方法与 `importFolder`/`importFiles` 公有签名加尾参 `const QMap<QString, QString> &nameOverrides = {}`）
- Modify: `src/domain/importrows.h`（`FolderPreviewRow` 加 `displayName` 字段，预览期填原文件名）
- Modify: `src/ui/dialogs/folderconfirm*.cpp/h`（确认表加可编辑「显示名」列；结果以 `nameOverrides` 读出）
- Modify: `src/workflow/folderimport.h/.cpp`（`importFolder`/`importFiles` 加尾参透传）
- Modify: `src/ui/paleomainwindow_project.cpp`（调用点组装 nameOverrides）
- Test: `tests/tst_import.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
void TestImport::batchNameOverridesApplied()
{
  const QString a = writeTestLas("E5_log.las");
  QString err;
  const auto rows = svc->importFiles({a}, &err, {}, {}, {},
                                     {{a, QStringLiteral("E5 井 * 新名字")}});
  QCOMPARE(rows.first().outcome, FolderRowResult::Outcome::Imported);
  QCOMPARE(svc->catalog()->assetById(rows.first().assetId).displayName,
           QString("E5 井 * 新名字"));
}
```

（`importFiles` 参数序按修改后签名对齐。）

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：编译失败（签名/字段不存在）。

- [ ] **Step 3: 实现**

`ingestplan.h` `PlannedItem` 加：

```cpp
  QString displayName;       // 确认表「显示名」列 override；空 = 原文件 basename
```

`dataimport_folder.cpp`：
- `produceFromPlan` 加形参 `const QMap<QString, QString> &nameOverrides`，在 override 并入段（typeOverrides 处理旁）加：

```cpp
  for (PlannedItem &it : plan.items)
  {
    const QString name = nameOverrides.value(it.path).trimmed();
    if (!name.isEmpty())
      it.displayName = name;
  }
```

- `produceItem` 在 `opts.forceType = item.type;` 旁加 `opts.displayName = item.displayName;`
- `produceFolder`/`produceFiles` 加同名形参透传 `produceFromPlan`；`dataimportservice.h` 中 `importFolder`（3 个重载）/`importFiles` 与对应静态 produce 声明同步加尾参（默认 `{}`，源文件兼容旧调用）。

`importrows.h` `FolderPreviewRow` 加 `QString displayName;`；`previewRowsFromPlan` 填 `row.displayName = QFileInfo(it.path).fileName();`（若该行结构有 `fileName` 字段则复用其值）。

`folderconfirm` 确认表：表格插入「显示名」可编辑列（默认 `displayName`，`Qt::ItemIsEditable`）；结果收集处产出 `QMap<QString,QString> nameOverrides`（源路径 → 编辑后名称，与原名不同才收录）。`paleomainwindow_project.cpp` 调用点把 map 传进 `importFiles/importFolder`。

`folderimport.h/.cpp`：`importFolder`/`importFiles` 各加尾参 `const QMap<QString, QString> &nameOverrides = {}`，produce lambda 捕获并透传到 `produceFolder/produceFiles`。

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_import && ctest --test-dir build -R '^tst_import$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src && git commit -m "feat(io): 批量确认表显示名列——nameOverrides 经 plan 贯通到资产 displayName"`

---

## 阶段二（功能二）：画线吸附 + 吸附即井序 + 空间重排

### Task 6: domain 纯算法——最近邻链排序 + 最近井查找

**Files:**
- Modify: `src/domain/wellsection.h`（`orderWellsByPosition` 声明 `:209` 旁）
- Modify: `src/domain/wellsection.cpp`（`orderWellsByPosition` 实现 `:474-538` 之后）
- Test: `tests/tst_wellsection.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
void TestWellsection::orderWellsNearestNeighborChains()
{
  QVector<Well> wells;
  auto mk = [](const QString &id, double x, double y) {
    Well w; w.id = id; w.x = x; w.y = y; /* hasCoordinates 真值按 Well 实际字段/方法设置 */ return w;
  };
  wells << mk("A", 0, 0) << mk("B", 10, 0) << mk("C", 20, 0) << mk("D", 5, 0);
  // 起点 = 最西端（x 最小）A；其后贪心：A→D(5)→B(10)→C(20)
  QCOMPARE(orderWellsNearestNeighbor({"A", "B", "C", "D"}, wells),
           QStringList({"A", "D", "B", "C"}));
  // 缺坐标井保原序排末（与 orderWellsByPosition 尾缀同口径）
  Well noCoord; noCoord.id = "E"; // hasCoordinates()==false
  wells << noCoord;
  QCOMPARE(orderWellsNearestNeighbor({"E", "A", "B", "C", "D"}, wells),
           QStringList({"A", "D", "B", "C", "E"}));
  // 单井/空集/全缺坐标 → 原序
  QCOMPARE(orderWellsNearestNeighbor({"A"}, wells), QStringList({"A"}));
  QCOMPARE(orderWellsNearestNeighbor({}, wells), QStringList({}));
}

void TestWellsection::nearestWellWithinTolerance()
{
  QVector<Well> wells;
  /* 同上 mk */ wells << mk("A", 0, 0) << mk("B", 100, 0);
  QCOMPARE(nearestWellWithin(wells, 3, 4, 10.0), 0);   // 距 A 为 5 < 10
  QCOMPARE(nearestWellWithin(wells, 50, 0, 10.0), -1); // 两都超容差
  QCOMPARE(nearestWellWithin(wells, 95, 0, 10.0), 1);  // 距 B 为 5
  QCOMPARE(nearestWellWithin(wells, 0, 0, 0.0), 0);    // 零距离零容差命中
}
```

（`Well` 的坐标有效性接口以 `src/domain/wellsection.h:105` 的实际定义为准——若有 `setCoordinates`/构造参数按它写。）

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_wellsection && ctest --test-dir build -R '^tst_wellsection$' --output-on-failure`
预期：编译失败（函数未声明）。

- [ ] **Step 3: 实现**

`wellsection.h`（`orderWellsByPosition` 声明旁）：

```cpp
// 最近邻链排序（连井剖面「按空间关系重排」算法一）：从最西端井（x 最小；
// 并列取 y 最小；再并列取输入序）起，每步取未访问井中平方距离最近者
// （距离并列取输入序靠前者——全确定性）。缺坐标井保原序排末
// （与 orderWellsByPosition 尾缀同口径）。
QStringList orderWellsNearestNeighbor(const QStringList &ids,
                                      const QVector<Well> &wellsWithCoords);
// 最近井查找（画线吸附用）：返回 (x,y) 的 tol 地图单位内平方距离最近的
// 有坐标井下标；并列取先出现者；无命中 → -1。
int nearestWellWithin(const QVector<Well> &wellsWithCoords, double x, double y,
                      double tolMapUnits);
```

`wellsection.cpp`（`orderWellsByPosition` 之后）：

```cpp
int nearestWellWithin(const QVector<Well> &wellsWithCoords, double x, double y,
                      double tolMapUnits) {
  int best = -1;
  double bestD2 = tolMapUnits * tolMapUnits;
  for (int i = 0; i < wellsWithCoords.size(); ++i) {
    const Well &w = wellsWithCoords.at(i);
    if (!w.hasCoordinates())
      continue;
    const double dx = w.x - x, dy = w.y - y;
    const double d2 = dx * dx + dy * dy;
    if (d2 <= bestD2 && (best < 0 || d2 < bestD2)) {
      best = i;
      bestD2 = d2;
    }
  }
  return best;
}

QStringList orderWellsNearestNeighbor(const QStringList &ids,
                                      const QVector<Well> &wellsWithCoords) {
  const auto locate = [&](const QString &id) -> const Well * {
    for (const Well &w : wellsWithCoords)
      if (w.id == id)
        return &w;
    return nullptr;
  };
  QVector<int> pending; // 有坐标井的 ids 下标
  QStringList tail;     // 缺坐标（保原序排末）
  for (int i = 0; i < ids.size(); ++i) {
    const Well *w = locate(ids.at(i));
    if (w && w->hasCoordinates())
      pending << i;
    else
      tail << ids.at(i);
  }
  QStringList out;
  if (pending.isEmpty())
    return ids;
  // 起点：最西端（x 最小；并列 y 最小；再并列输入序）
  int start = 0;
  for (int k = 1; k < pending.size(); ++k) {
    const Well *a = locate(ids.at(pending.at(k)));
    const Well *b = locate(ids.at(pending.at(start)));
    if (a->x < b->x || (a->x == b->x && a->y < b->y))
      start = k;
  }
  QVector<bool> used(pending.size(), false);
  used[start] = true;
  out << ids.at(pending.at(start));
  const Well *cur = locate(ids.at(pending.at(start)));
  for (int step = 1; step < pending.size(); ++step) {
    int next = -1;
    double bestD2 = std::numeric_limits<double>::max();
    for (int k = 0; k < pending.size(); ++k) {
      if (used.at(k))
        continue;
      const Well *cand = locate(ids.at(pending.at(k)));
      const double dx = cand->x - cur->x, dy = cand->y - cur->y;
      const double d2 = dx * dx + dy * dy;
      if (d2 < bestD2) {
        bestD2 = d2;
        next = k;
      }
    }
    used[next] = true;
    out << ids.at(pending.at(next));
    cur = locate(ids.at(pending.at(next)));
  }
  out << tail;
  return out;
}
```

（`#include <limits>` 按文件头现存情况补。）

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_wellsection && ctest --test-dir build -R '^tst_wellsection$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/domain tests/tst_wellsection.cpp && git commit -m "feat(domain): 连井空间算法——最近邻链排序 orderWellsNearestNeighbor + 最近井查找 nearestWellWithin"`

---

### Task 7: `SectionWorkbench` 空间排序与吸附井集

**Files:**
- Modify: `src/workflow/sectionworkbench.h`
- Modify: `src/workflow/sectionworkbench.cpp`（`wellRoute` `:254-280` 附近同文件加方法）
- Test: `tests/tst_wellsection_workflow.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
void TestWellsectionWorkflow::spatiallyOrderedWellsModes()
{
  // 沿用文件内既有临时工程 + catalog 建井 fixture（surfaceX/Y 有坐标）
  // 井：W1(0,0) W2(10,0) W3(20,0) W4(5,0)
  SectionWorkbench wb(cat);
  QCOMPARE(wb.spatiallyOrderedWells({"W1", "W2", "W3", "W4"}, "nearest"),
           QStringList({"W1", "W4", "W2", "W3"}));
  const QStringList pca = wb.spatiallyOrderedWells({"W3", "W1", "W4", "W2"}, "pca");
  QCOMPARE(pca, QStringList({"W1", "W4", "W2", "W3"})); // 共线井主轴投影=x 序
  // 未知 mode → 原序
  QCOMPARE(wb.spatiallyOrderedWells({"W2", "W1"}, "bogus"), QStringList({"W2", "W1"}));
}

void TestWellsectionWorkflow::wellSnapWellsOnlyValidCoords()
{
  // 有坐标井入选；coordinateStatus=="invalid"/无 surface 的井排除
  const auto pts = wb.wellSnapWells();
  QCOMPARE(pts.size(), 2);
  QCOMPARE(pts.first().id, QString("W1"));
  QCOMPARE(pts.first().x, 0.0);
}
```

（井 id 以 fixture 实际建井口径为准——`wellRoute` 用 catalog 实体 id，测试照它。）

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_wellsection_workflow && ctest --test-dir build -R '^tst_wellsection_workflow$' --output-on-failure`
预期：编译失败。

- [ ] **Step 3: 实现**

`sectionworkbench.h`（`wellRoute` 声明旁）：

```cpp
  // 「按空间关系重排」：mode "nearest"=最近邻链 / "pca"=主轴投影（domain
  // orderWellsNearestNeighbor / orderWellsByPosition）；井坐标取 catalog 井
  // 实体（coordinateStatus ok/untransformed 且有限，与 wellRoute 同口径）。
  // 未知 mode 原样返回。
  QStringList spatiallyOrderedWells(const QStringList &ids, const QString &mode);
  // 画线吸附井集：有效坐标井（口径同上）的 id/x/y，供 SeismicSectionTool。
  QVector<Well> wellSnapWells();
```

`sectionworkbench.cpp`：

```cpp
#include "../domain/wellsection.h"  // 文件头include区按现存顺序插入

QStringList SectionWorkbench::spatiallyOrderedWells(const QStringList &ids,
                                                    const QString &mode) {
  const QVector<Well> wells = wellSnapWells();
  if (mode == QLatin1String("nearest"))
    return orderWellsNearestNeighbor(ids, wells);
  if (mode == QLatin1String("pca"))
    return orderWellsByPosition(ids, wells);
  return ids;
}

QVector<Well> SectionWorkbench::wellSnapWells() {
  QVector<Well> out;
  if (!m_catalog)
    return out;
  for (const CatalogEntity &e : m_catalog->entities(QStringLiteral("well"))) {
    if (!e.hasSurface || !std::isfinite(e.surfaceX) || !std::isfinite(e.surfaceY))
      continue;
    if (e.coordinateStatus != QLatin1String("ok") &&
        e.coordinateStatus != QLatin1String("untransformed"))
      continue;
    Well w;
    w.id = e.id;
    w.x = e.surfaceX;
    w.y = e.surfaceY;
    // hasCoordinates 的真值口径按 Well 实际字段/方法设置（若无显式字段，
    // 以 x/y 赋值即有效的现有构造为准——参照 orderWellsByPosition 调用方
    // 的 Well 构造方式，如 wellsectionpanel.cpp:1350-1387 区域）。
    out << w;
  }
  return out;
}
```

（关键：先读 `src/domain/wellsection.h:105` 的 `Well` 定义与 `src/ui/wellsection/wellsectionpanel.cpp:1350-1387` 的既有构造方式，照抄同一真值口径。）

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_wellsection_workflow && ctest --test-dir build -R '^tst_wellsection_workflow$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/workflow tests/tst_wellsection_workflow.cpp && git commit -m "feat(workflow): SectionWorkbench 空间排序（nearest/pca）与吸附井集 wellSnapWells"`

---

### Task 8: `SeismicSectionTool` 井位吸附与井序收集

**Files:**
- Modify: `src/qgis/seismicsectiontool.h`
- Modify: `src/qgis/seismicsectiontool.cpp`
- Modify: `src/linkage/seismicmaplink.h:61` + `src/linkage/seismicmaplink.cpp:253-267`（`onSectionPathCaptured` 加 `wellIds` 参）
- Test: 纯逻辑已被 Task 6 覆盖；本任务靠 Task 10 的对话框联动测试与手测（QGIS 画布鼠标事件合成成本高，既有 `tst_sectionlifecycle` 保证生命周期不回归——跑它）

- [ ] **Step 1: 改工具（头文件）**

```cpp
// 层：QGIS 封装
#pragma once

#include <qgsmaptool.h>
#include <qgspointxy.h>
#include <qgsrubberband.h>
#include <QVector>
#include <QStringList>

#include "../domain/wellsection.h" // Well（吸附最近邻纯算法在 domain，可单测）

class QgsVertexMarker;

// Interactive map tool for capturing an arbitrary section polyline directly on the QgsMapCanvas.
// Left click adds points; right click finishes capture and emits sectionPathCaptured.
// 井位吸附（方向：连井剖面吸附成线）：setSnapWells 注入有效井集后，移动时
// 容差内最近井吸附预览（顶点标记 + 画布 tooltip 井名）；吸附落点取井口
// 精确坐标并把 well id 按顺序收集（连续同井忽略），随完成信号发出。
class SeismicSectionTool : public QgsMapTool
{
  Q_OBJECT

public:
  explicit SeismicSectionTool(QgsMapCanvas *canvas);
  ~SeismicSectionTool() override;

  void activate() override;
  void deactivate() override;

  // 吸附井集（有效坐标井；空集 = 关闭吸附，行为同旧纯任意线）。
  void setSnapWells(const QVector<Well> &wells);

signals:
  // 完成：折线顶点 + 吸附井的有序 id 列表（<2 口时壳按纯任意线处理）。
  void sectionPathCaptured(const QVector<QgsPointXY> &points, const QStringList &wellIds);
  // Esc 取消：壳据此把为绘制而隐藏的设置对话框唤回。
  void captureCancelled();

protected:
  void canvasPressEvent(QgsMapMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void canvasMoveEvent(QgsMapMouseEvent *e) override;

private:
  void redraw(const QgsPointXY *hover = nullptr);
  void clearSnapFeedback();
  double snapToleranceMapUnits() const;
  QVector<QgsPointXY> m_points;
  QStringList m_vertexWellIds; // 与 m_points 等长；自由顶点为空串
  QVector<Well> m_snapWells;
  int m_hoveredWell = -1;
  QgsRubberBand *m_rubberBand = nullptr;
  QgsVertexMarker *m_snapMarker = nullptr;
};
```

- [ ] **Step 2: 改工具（实现）**

```cpp
// 层：QGIS 封装
#include "seismicsectiontool.h"

#include <QCursor>
#include <QKeyEvent>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsvertexmarker.h>
#include <qgssnappingutils.h>

SeismicSectionTool::SeismicSectionTool(QgsMapCanvas *canvas)
  : QgsMapTool(canvas)
{
  setCursor(QCursor(Qt::CrossCursor));
  if (canvas)
  {
    m_rubberBand = new QgsRubberBand(canvas, Qgis::GeometryType::Line);
    m_rubberBand->setColor(QColor(QStringLiteral("#1B73D0")));
    m_rubberBand->setWidth(2);
    m_rubberBand->setLineStyle(Qt::DashLine);
    m_snapMarker = new QgsVertexMarker(canvas);
    m_snapMarker->setIconType(QgsVertexMarker::ICON_CIRCLE);
    m_snapMarker->setColor(QColor(QStringLiteral("#D08C1B")));
    m_snapMarker->setIconSize(12);
    m_snapMarker->setPenWidth(2);
    m_snapMarker->hide();
  }
}

SeismicSectionTool::~SeismicSectionTool()
{
  // 橡皮带与顶点标记属 canvas scene（决策①，PreviewMapCanvas 同款）：
  // canvas 先死时 delete 即悬空 SIGSEGV——scene 自会清理自己的项，工具不越权。
}

void SeismicSectionTool::setSnapWells(const QVector<Well> &wells)
{
  m_snapWells = wells;
}

void SeismicSectionTool::activate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  m_hoveredWell = -1;
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::activate();
}

void SeismicSectionTool::deactivate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  m_hoveredWell = -1;
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::deactivate();
}

double SeismicSectionTool::snapToleranceMapUnits() const
{
  // 容差取画布 QGIS 吸附设置的像素容差，读取不到/未启用时回退 12 像素。
  double pixels = 12.0;
  if (canvas() && canvas()->snappingUtils())
  {
    const QgsSnappingConfig cfg = canvas()->snappingUtils()->config();
    if (cfg.enabled() && cfg.type() == Qgis::SnappingType::Vertex)
      pixels = cfg.tolerance() > 0 ? cfg.tolerance() : pixels;
    else if (cfg.enabled())
      pixels = cfg.tolerance() > 0 ? cfg.tolerance() : pixels;
  }
  return pixels * (canvas() ? canvas()->mapUnitsPerPixel() : 1.0);
}

void SeismicSectionTool::clearSnapFeedback()
{
  if (m_snapMarker)
    m_snapMarker->hide();
  if (canvas())
    canvas()->unsetToolTip();
}

void SeismicSectionTool::canvasPressEvent(QgsMapMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
  {
    if (m_hoveredWell >= 0 && m_hoveredWell < m_snapWells.size())
    {
      const Well &w = m_snapWells.at(m_hoveredWell);
      // 连续同井忽略（不产生重复顶点与重复井序项）
      if (m_vertexWellIds.isEmpty() || m_vertexWellIds.last() != w.id)
      {
        m_points.append(QgsPointXY(w.x, w.y));
        m_vertexWellIds.append(w.id);
      }
    }
    else
    {
      m_points.append(e->mapPoint());
      m_vertexWellIds.append(QString());
    }
    redraw();
  }
  else if (e->button() == Qt::RightButton)
  {
    if (m_points.size() >= 2)
    {
      const QVector<QgsPointXY> captured = m_points;
      QStringList wellIds;
      for (const QString &id : m_vertexWellIds)
        if (!id.isEmpty())
          wellIds << id;
      m_points.clear();
      m_vertexWellIds.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
      emit sectionPathCaptured(captured, wellIds);
    }
    else
    {
      m_points.clear();
      m_vertexWellIds.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
    }
  }
}

void SeismicSectionTool::canvasMoveEvent(QgsMapMouseEvent *e)
{
  const auto pt = e->mapPoint();
  m_hoveredWell = m_snapWells.isEmpty()
                      ? -1
                      : nearestWellWithin(m_snapWells, pt.x(), pt.y(),
                                          snapToleranceMapUnits());
  QgsPointXY hover = pt;
  if (m_hoveredWell >= 0)
  {
    const Well &w = m_snapWells.at(m_hoveredWell);
    hover = QgsPointXY(w.x, w.y);
    if (m_snapMarker)
    {
      m_snapMarker->setCenter(hover);
      m_snapMarker->show();
    }
    if (canvas())
      canvas()->setToolTip(w.id); // Well 无 name 字段时显示 id；壳侧井名一致
  }
  else
  {
    clearSnapFeedback();
  }
  if (m_points.isEmpty() || !m_rubberBand)
    return;
  redraw(&hover);
}

void SeismicSectionTool::redraw(const QgsPointXY *hover) {
  if (!m_rubberBand)
    return;
  m_rubberBand->reset(Qgis::GeometryType::Line);
  for (const auto &p : m_points)
    m_rubberBand->addPoint(p, false);
  if (hover)
    m_rubberBand->addPoint(*hover, false);
  m_rubberBand->show();
  m_rubberBand->updatePosition();
}

void SeismicSectionTool::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Escape) {
    m_points.clear();
    m_vertexWellIds.clear();
    m_hoveredWell = -1;
    clearSnapFeedback();
    redraw();
    emit captureCancelled();
    if (canvas())
      canvas()->unsetMapTool(this);
  } else if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
    if (!m_points.isEmpty())
    {
      m_points.removeLast();
      m_vertexWellIds.removeLast();
    }
    redraw();
  } else
    QgsMapTool::keyPressEvent(e);
}
```

（`QgsSnappingConfig` 的 `type()/tolerance()` 成员名以 vendored QGIS 4.2 头为准——若字段名有出入，回退硬编码 12 像素并在代码注释写明原因。`Well` 若有 `name` 字段，tooltip 用 `w.name`。）

- [ ] **Step 3: `SeismicMapLink::onSectionPathCaptured` 加参**

`seismicmaplink.h:61`：

```cpp
    // 工具路径回调（R4 信号化后由壳 connect）。wellIds = 吸附收集的有序井
    // （≥2 口时默认标题取「连井剖面 · N 口井」，否则「地图拾取任意剖面」）。
    void onSectionPathCaptured(const QVector<QgsPointXY> &points,
                               const QStringList &wellIds = {});
```

`seismicmaplink.cpp:253-267` 尾部调用改为：

```cpp
  triggerSectionFromMapPolyline(
      local, wellIds.size() >= 2
                 ? tr("连井剖面 · %1 口井").arg(wellIds.size())
                 : tr("地图拾取任意剖面"));
```

- [ ] **Step 4: 编译 + 生命周期回归**

`cmake --build build -j8 --target paleo_qgis && cmake --build build -j8 --target tst_sectionlifecycle && ctest --test-dir build -R 'tst_sectionlifecycle|tst_sections_alignment' --output-on-failure`
预期：编译过（壳的 connect 暂时签名不匹配——Task 9 同步改，故此处允许壳目标暂时编译失败；若必须整树绿才能跑测试，先完成 Task 9 Step 1 再跑本步）。

- [ ] **Step 5: 提交**

`git add src/qgis src/linkage && git commit -m "feat(qgis): 剖面画线井位吸附——容差内最近井吸附预览、吸附顶点收集井序、信号携带有序 wellIds"`

---

### Task 9: 壳接线——吸附供给、完成同步、成功唤回对话框

**Files:**
- Modify: `src/ui/paleomainwindow_sections.cpp:224-242`（工具创建/激活处）与 `:140-153`（提取完成处）

- [ ] **Step 1: 实现**

工具创建 connect（`:224-242`）改为：

```cpp
  connect(link, &SeismicMapLink::sectionCaptureRequested, this, [this, workbench, setup] {
    QgsMapCanvas *canvas = m_canvasCtl ? m_canvasCtl->canvas() : nullptr;
    if (!canvas)
      return;
    if (!m_sectionCaptureTool)
    {
      m_sectionCaptureTool = new SeismicSectionTool(canvas);
      connect(m_sectionCaptureTool, &SeismicSectionTool::sectionPathCaptured,
              m_sectionLink, &SeismicMapLink::onSectionPathCaptured);
      // 吸附成线的井序同步回设置对话框（≥2 口 → 勾选与顺序随吸附序）。
      connect(m_sectionCaptureTool, &SeismicSectionTool::sectionPathCaptured,
              setup, [setup](const QVector<QgsPointXY> &, const QStringList &wellIds) {
                if (wellIds.size() >= 2)
                  setup->applyWellOrder(wellIds);
              });
      connect(m_sectionCaptureTool, &SeismicSectionTool::captureCancelled,
              m_sectionLink, &SeismicMapLink::sectionCaptureCancelled);
      connect(canvas, &QObject::destroyed, this, [this]() {
        m_sectionCaptureTool = nullptr;
      });
    }
    m_sectionCaptureTool->setSnapWells(workbench->wellSnapWells()); // 每次激活前刷新井集
    canvas->setMapTool(m_sectionCaptureTool);
  });
```

（`setup`/`workbench` 是 `attachSections` 内既有局部变量，捕获进 lambda。注意原 connect 只捕获 `this`——替换整个 lambda。）

提取完成 connect（`:140-153`）改为成功失败都唤回（D2「快速形成后继续对齐」）：

```cpp
  connect(link, &SeismicMapLink::sectionExtractedFromMap, this,
          [report, setup, drawingFromSetup](bool ok, const QString &error) {
            if (*drawingFromSetup)
            {
              *drawingFromSetup = false;
              setup->show();   // 成功也唤回：接着做时深对齐/保存版本
              setup->raise();
            }
            if (!ok)
              report(error);
          });
```

- [ ] **Step 2: 编译 + 相关测试**

`cmake --build build -j8 && ctest --test-dir build -R 'tst_sectionlifecycle|tst_sections_alignment' --output-on-failure`
预期：PASS。

- [ ] **Step 3: 提交**

`git add src/ui/paleomainwindow_sections.cpp && git commit -m "feat(ui): 壳接线剖面吸附——激活前注入井集、吸附井序同步对话框、提取成功即唤回设置对话框"`

---

### Task 10: 对话框——`applyWellOrder` / 空间重排按钮

**Files:**
- Modify: `src/ui/seismicsection/sectionsetupdialog.h`
- Create: `src/ui/seismicsection/sectionsetupdialog_order.cpp`（部分类拆分先例，规避 ui_size/file_budget 闸）
- Modify: `src/ui/seismicsection/sectionsetupdialog.cpp`（构造函数加重排按钮；gateBuild 联动）
- Modify: `src/ui/paleomainwindow_sections.cpp`（spatialReorderRequested 接线）
- Test: `tests/tst_sections_alignment.cpp`（若无对话框既有用例，按该文件既有 fixture 新建）

- [ ] **Step 1: 写失败测试**

```cpp
void TestSectionsAlignment::applyWellOrderReordersAndChecks()
{
  SectionSetupDialog dlg;
  QVariantList wells;
  auto mk = [](const QString &id) {
    QVariantMap m; m["id"] = id; m["name"] = id; m["status"] = "ok";
    m["coordinates"] = true; return m;
  };
  wells << mk("A") << mk("B") << mk("C") << mk("D");
  dlg.setWells(wells);
  dlg.applyWellOrder({"C", "A"});
  QCOMPARE(dlg.checkedWellIds(), QStringList({"C", "A"})); // 顺序与勾选随吸附序
}

void TestSectionsAlignment::spatialReorderButtonGateAndSignal()
{
  SectionSetupDialog dlg;
  /* 同上 setWells 4 口 */
  auto *btn = dlg.findChild<QPushButton *>("spatialReorderBtn");
  QVERIFY(btn);
  QVERIFY(!btn->isEnabled()); // 勾选 <2 禁用
  dlg.applyWellOrder({"B", "D"});
  QVERIFY(btn->isEnabled());
  QSignalSpy spy(&dlg, &SectionSetupDialog::spatialReorderRequested);
  // 触发菜单中「最近邻链」动作（objectName spatialReorderNearest）
  dlg.findChild<QAction *>("spatialReorderNearest")->trigger();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(0).toString(), QString("nearest"));
  QCOMPARE(spy.first().at(1).toStringList(), QStringList({"B", "D"}));
}
```

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_sections_alignment && ctest --test-dir build -R '^tst_sections_alignment$' --output-on-failure`
预期：编译失败（方法/信号不存在）。

- [ ] **Step 3: 实现**

`sectionsetupdialog.h`：

```cpp
// 层：视图
#pragma once
#include <QDialog>
#include <QVariantList>
#include <QStringList>
class QListWidget;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
class QLineEdit;
class SectionSetupDialog : public QDialog {
  Q_OBJECT
public:
  explicit SectionSetupDialog(QWidget *parent = nullptr);
  void setWells(const QVariantList &wells);
  void setSavedSections(const QVariantList &sections);
  void setMessage(const QString &text);
  // 吸附成线/空间重排的井序落盘：ids 按给定序置顶并勾选，其余井保持
  // 当前相对序、置未勾选（sectionsetupdialog_order.cpp）。
  void applyWellOrder(const QStringList &ids);
  // 当前勾选井 id（列表序）。
  QStringList checkedWellIds() const;
signals:
  void buildRequested(const QStringList &ids);
  void drawRequested();
  // 清除地图上的剖面连线（rubber band + 壳侧路线状态）。
  void clearRequested();
  // 「按空间关系重排」：mode = "nearest" | "pca"；ids = 当前勾选井（列表序）。
  void spatialReorderRequested(const QString &mode, const QStringList &ids);
  void calibrationRequested(const QString &id, bool constant, double velocity,
                            double shift);
  void saveRequested(const QString &name);
  void restoreRequested(const QString &versionId);

private:
  void showWell();
  QListWidget *m_wells;
  QComboBox *m_mode, *m_saved;
  QDoubleSpinBox *m_velocity, *m_shift;
  QLabel *m_status, *m_message;
  QTableWidget *m_samples;
  QLineEdit *m_name;
};
```

`sectionsetupdialog.cpp` 构造函数（`clear` 按钮之后、right group 之前）加：

```cpp
  auto *reorder = new QPushButton(tr("按空间关系重排"), this);
  reorder->setObjectName("spatialReorderBtn");
  reorder->setToolTip(tr("对勾选井按井位空间关系自动排序：最近邻链或沿主轴投影"));
  auto *reorderMenu = new QMenu(reorder);
  auto *nearestAct = reorderMenu->addAction(tr("最近邻链（从最西端起贪心连接）"));
  nearestAct->setObjectName("spatialReorderNearest");
  auto *pcaAct = reorderMenu->addAction(tr("沿主轴投影排序"));
  pcaAct->setObjectName("spatialReorderPca");
  reorder->setMenu(reorderMenu);
  left->addWidget(reorder);
  connect(nearestAct, &QAction::triggered, this,
          [this] { emit spatialReorderRequested(QStringLiteral("nearest"), checkedWellIds()); });
  connect(pcaAct, &QAction::triggered, this,
          [this] { emit spatialReorderRequested(QStringLiteral("pca"), checkedWellIds()); });
```

`gateBuild` lambda 联动（勾选数同样门控 reorder 按钮；把 `build` 捕获改为同时捕获 `reorder`）：

```cpp
  auto gateBuild = [this, build, reorder] {
    int selected = 0;
    for (int i = 0; i < m_wells->count(); ++i)
      selected += m_wells->item(i)->checkState() == Qt::Checked;
    build->setEnabled(selected >= 2);
    build->setToolTip(selected < 2 ? tr("至少勾选两口坐标不同的井")
                                   : tr("按列表从上到下生成剖面"));
    reorder->setEnabled(selected >= 2);
  };
```

（`reorder` 声明须在 `gateBuild` 定义之前——把按钮创建代码移到 gateBuild 之前。）

新建 `sectionsetupdialog_order.cpp`：

```cpp
// 层：视图
// SectionSetupDialog 部分类拆分（datalist_* 先例）：井序应用与勾选查询。
#include "sectionsetupdialog.h"
#include <QListWidget>
#include <QSignalBlocker>

void SectionSetupDialog::applyWellOrder(const QStringList &ids) {
  QMap<QString, QVariantMap> rows;
  QStringList rest;
  for (int i = 0; i < m_wells->count(); ++i) {
    auto *item = m_wells->item(i);
    const auto row = item->data(Qt::UserRole).toMap();
    const QString id = row.value("id").toString();
    rows[id] = row;
    if (!ids.contains(id))
      rest << id;
  }
  const QSignalBlocker block(m_wells);
  m_wells->clear();
  auto addRow = [this, &rows](const QString &id, bool checked) {
    const auto row = rows.value(id);
    auto *item = new QListWidgetItem(row.value("name").toString(), m_wells);
    item->setData(Qt::UserRole, row);
    item->setToolTip(row.value("status").toString());
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    if (!row.value("coordinates").toBool()) {
      item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
      item->setToolTip(tr("缺少有效井口坐标"));
    }
  };
  for (const QString &id : ids)
    if (rows.contains(id))
      addRow(id, true);
  for (const QString &id : rest)
    addRow(id, false);
  if (m_wells->count())
    m_wells->setCurrentRow(0);
  showWell();
  // 与 setWells 末尾同口径：手工刷新生成按钮门控（itemChanged 被 Blocker 抑制）
  int checkedCount = 0;
  for (int i = 0; i < m_wells->count(); ++i)
    checkedCount += m_wells->item(i)->checkState() == Qt::Checked;
  findChild<QPushButton *>("buildWellSection")->setEnabled(checkedCount >= 2);
  if (auto *reorder = findChild<QPushButton *>("spatialReorderBtn"))
    reorder->setEnabled(checkedCount >= 2);
}

QStringList SectionSetupDialog::checkedWellIds() const {
  QStringList ids;
  for (int i = 0; i < m_wells->count(); ++i)
    if (m_wells->item(i)->checkState() == Qt::Checked)
      ids << m_wells->item(i)->data(Qt::UserRole).toMap().value("id").toString();
  return ids;
}
```

（`#include <QMap>` / `QPushButton` 按需要补；CMake 把新文件加进 ui 库源清单——`grep -rn "sectionsetupdialog.cpp" CMakeLists.txt cmake/ src/ | grep -v _order` 定位。）

`paleomainwindow_sections.cpp`（buildRequested connect 附近）加：

```cpp
  connect(setup, &SectionSetupDialog::spatialReorderRequested, this,
          [workbench, setup](const QString &mode, const QStringList &ids) {
            setup->applyWellOrder(workbench->spatiallyOrderedWells(ids, mode));
          });
```

- [ ] **Step 4: 跑绿 + 闸门**

`cmake --build build -j8 --target tst_sections_alignment && ctest --test-dir build -R '^tst_sections_alignment$' --output-on-failure && ctest --test-dir build -R '^(layering|ui_size|file_budget|ui_invariants)$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/ui tests/tst_sections_alignment.cpp CMakeLists.txt cmake && git commit -m "feat(ui): 连井设置对话框——吸附井序落盘 applyWellOrder + 按空间关系重排（最近邻链/主轴投影）"`

---

## 阶段三（功能三）：删除索引（文件一律不动）+ 孤儿治理

### Task 11: `paleo::assetops::removeAssetsKeepFiles` + `scanOrphanManagedDirs`

**Files:**
- Modify: `src/workflow/assetops.h`
- Modify: `src/workflow/assetops.cpp`
- Test: `tests/tst_assetops.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
void TestAssetops::removeKeepFilesDeletesIndexOnly()
{
  // 沿用文件内既有 fixture：临时工程 + catalog + 造受管资产（artifacts/raw 有文件）
  // + 外链资产（工程外文件）
  const QString managedFile = /* 受管版本绝对路径（resolvedVersionPath） */;
  const QString externalFile = /* 外链源路径 */;
  QVERIFY(QFile::exists(managedFile));
  QVERIFY(QFile::exists(externalFile));
  QString err;
  QVERIFY(paleo::assetops::removeAssetsKeepFiles(&cat, {managedAssetId, externalAssetId}, &err));
  QVERIFY(cat.assetById(managedAssetId).id.isEmpty());   // 索引删了
  QVERIFY(cat.assetById(externalAssetId).id.isEmpty());
  QVERIFY(QFile::exists(managedFile));                   // 文件一律不动
  QVERIFY(QFile::exists(externalFile));
  // 血缘保护透传：被派生引用的资产拒绝且索引不动
  QVERIFY(!paleo::assetops::removeAssetsKeepFiles(&cat, {lineageParentId}, &err));
  QVERIFY(!cat.assetById(lineageParentId).id.isEmpty());
}

void TestAssetops::scanOrphanManagedDirsFindsUntracked()
{
  // 在 artifacts/raw 下造两个资产目录：一个在 catalog（建资产）、一个不在
  const auto orphans = paleo::assetops::scanOrphanManagedDirs(dir, &cat);
  QCOMPARE(orphans.size(), 1);
  QCOMPARE(orphans.first().relPath, QString("raw/%1").arg(orphanAssetDir));
  QVERIFY(orphans.first().bytes > 0);
}
```

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_assetops && ctest --test-dir build -R '^tst_assetops$' --output-on-failure`
预期：编译失败（函数不存在）。

- [ ] **Step 3: 实现**

`assetops.h`（`purgeManagedFiles` 声明之后）：

```cpp
// ---- 索引级删除（方向 D4：只删数据对象，文件一律不动）----
// 从目录移除资产索引（资产行+版本行+链接行，单事务；血缘/staging 保护同
// removeAssets）。不做任何文件操作：外链源文件与工程内受管副本都保留——
// 受管副本自此成为孤儿，由 scanOrphanManagedDirs/治理面板发现回收。
bool removeAssetsKeepFiles(DataCatalog *cat, const QStringList &assetIds,
                           QString *error = nullptr);

// ---- 孤儿受管文件扫描（治理面板「孤立受管文件」的数据面）----
struct OrphanManagedDir
{
  QString relPath;   // 相对 artifacts/，如 "raw/ast-12"
  QString absPath;
  qint64 bytes = 0;
  int fileCount = 0;
};
// 遍历 <projectDir>/artifacts/{raw,derived,intermediate,output}/<assetId>
// 一级目录：assetId 不在 catalog 资产表 → 孤儿。纯读盘 + catalog 只读查询，
// 可在任意线程跑（catalog 读纪律照旧：活对象只能在属主线程——调用方注意）。
QVector<OrphanManagedDir> scanOrphanManagedDirs(const QString &projectDir,
                                                const DataCatalog *cat);
```

`assetops.cpp`（文件尾 namespace 结束前）：

```cpp
bool removeAssetsKeepFiles(DataCatalog *cat, const QStringList &assetIds, QString *error)
{
  if (!cat)
  {
    if (error) *error = QStringLiteral("catalog is null");
    return false;
  }
  return cat->removeAssets(assetIds, error);
}

QVector<OrphanManagedDir> scanOrphanManagedDirs(const QString &projectDir,
                                                const DataCatalog *cat)
{
  QVector<OrphanManagedDir> out;
  if (projectDir.isEmpty() || !cat)
    return out;
  QSet<QString> known;
  for (const CatalogAsset &a : cat->assets())
    known.insert(a.id);
  const QStringList stages{QStringLiteral("raw"), QStringLiteral("derived"),
                           QStringLiteral("intermediate"), QStringLiteral("output")};
  for (const QString &stage : stages)
  {
    const QDir stageDir(projectDir + QStringLiteral("/artifacts/") + stage);
    if (!stageDir.exists())
      continue;
    const QStringList sub = stageDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &assetId : sub)
    {
      if (known.contains(assetId))
        continue;
      OrphanManagedDir d;
      d.relPath = stage + QLatin1Char('/') + assetId;
      d.absPath = stageDir.absoluteFilePath(assetId);
      QDirIterator it(d.absPath, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
      {
        it.next();
        d.bytes += static_cast<qint64>(it.fileInfo().size());
        ++d.fileCount;
      }
      out << d;
    }
  }
  return out;
}
```

（`QDir`/`QDirIterator` include 按文件头现存情况补。）

- [ ] **Step 4: 跑绿**

`cmake --build build -j8 --target tst_assetops && ctest --test-dir build -R '^tst_assetops$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/workflow tests/tst_assetops.cpp && git commit -m "feat(workflow): 索引级删除 removeAssetsKeepFiles（文件一律不动）+ 孤儿受管目录扫描 scanOrphanManagedDirs"`

---

### Task 12: 「从目录移除（保留文件）…」菜单入口

**Files:**
- Modify: `src/ui/pages/dataopspanelops.h:443-448`（矩阵加动作）
- Modify: `src/ui/pages/datalist_batch.cpp:520-563`（kTitles）与 dispatch
- Modify: `src/ui/pages/datalist_indexops.cpp`（实现 `removeFromCatalogKeepFiles`）
- Test: `tests/tst_panels_dataops.cpp`（矩阵期望更新——与 Task 2 同一断言区，一并核对）

- [ ] **Step 1: 更新矩阵测试期望**

`contextMenuActions` 资产面期望列表统一加 `"removeFromCatalog"`（紧随 `"removeSoft"` 之后；单资产无 `removeSoft` 时缀在 `"exportManifest"` 位——按实际矩阵序断言）。

- [ ] **Step 2: 跑红**

`cmake --build build -j8 --target tst_panels_dataops && ctest --test-dir build -R '^tst_panels_dataops$' --output-on-failure`
预期：矩阵断言失败。

- [ ] **Step 3: 实现**

`dataopspanelops.h` `contextMenuActions`（`:446-447` 之后）：

```cpp
    if (spec.assetCount > 1)
      acts << QStringLiteral("exportManifest") << QStringLiteral("removeSoft");
    acts << QStringLiteral("removeFromCatalog"); // 方向 D4：删索引保文件
```

`datalist_batch.cpp` kTitles 加：

```cpp
    {"removeFromCatalog", QT_TR_NOOP("从目录移除（保留文件）…")},
```

dispatch 加：

```cpp
  else if (key == QLatin1String("removeFromCatalog"))
    removeFromCatalogKeepFiles();
```

`datalist_indexops.cpp` 加实现：

```cpp
void DataListPanel::removeFromCatalogKeepFiles()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid() || m_ctx.cat->refusesWrites())
    return;
  if (!PaleoNotify::ask(this, tr("从目录移除（保留文件）"),
                        tr("将从目录移除 %1 项数据对象的索引记录（含挂接关系）：\n"
                           "· 所有本地文件一律不动（外链源文件与工程内受管副本均保留）；\n"
                           "· 工程内受管副本将成为孤立文件，可在存储治理中查看与回收；\n"
                           "· 此操作不可撤销——如需可恢复，请改用「删除（可恢复）…」。\n"
                           "确定继续？").arg(ids.size())))
    return;
  QString err;
  const QStringList list(ids.constBegin(), ids.constEnd());
  if (!paleo::assetops::removeAssetsKeepFiles(m_ctx.cat, list, &err))
  {
    PaleoNotify::warn(this, tr("从目录移除失败"), err);
    return;
  }
  if (m_history)
    m_history->push(tr("从目录移除 %1 项（保留文件）").arg(list.size()));
  emit statusMessage(tr("已从目录移除 %1 项索引记录；文件均未改动").arg(list.size()));
}
```

（`PaleoNotify::warn` 的确切静态面以 `paleonotify.h` 现存方法为准——无 warn 则用既有 ask/信息面。）

- [ ] **Step 4: 跑绿 + 闸门**

`cmake --build build -j8 --target tst_panels_dataops && ctest --test-dir build -R '^(tst_panels_dataops|layering.*)$' --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

`git add src/ui tests/tst_panels_dataops.cpp && git commit -m "feat(dataops): 「从目录移除（保留文件）…」——索引级删除入口，确认文案明示不可撤销与孤儿去向"`

---

### Task 13: 治理面板「孤立受管文件」扫描项

**Files:**
- Modify: `src/workflow/storagegovernancecontroller.h/.cpp`（加 `scanOrphans()` 槽 + `orphansReady` 信号；内部调 `paleo::assetops::scanOrphanManagedDirs`）
- Modify: `src/ui/dialogs/storagegovernancedialog.h/.cpp`（加「孤立受管文件」区块：表格 + 删除选中 + 在文件管理器中显示）
- Modify: `src/ui/pages/datalist_undo.cpp:600-650`（治理对话框与控制器接线区，补 orphans 连接）
- Test: 扫描数据面已在 Task 11 覆盖；本任务 UI 接线靠手测 + 既有治理测试不回归（先 `grep -rln "StorageGovernance" tests/` 找既有测试一并跑）

- [ ] **Step 1: 控制器加扫描面**

`storagegovernancecontroller.h`：

```cpp
  // 「孤立受管文件」扫描（方向 D4 留待治理）：artifacts 各 stage 下无
  // catalog 对应的资产目录。结果经 orphansReady 回报。
  void scanOrphans();
signals:
  void orphansReady(const QVector<paleo::assetops::OrphanManagedDir> &orphans);
```

`.cpp`（同步实现即可——扫描是毫秒级读盘；若既有 scan 是异步则照其模式）：

```cpp
void StorageGovernanceController::scanOrphans()
{
  emit orphansReady(paleo::assetops::scanOrphanManagedDirs(m_projectDir, m_catalog));
}
```

（成员名以该文件实际持有的工程目录/catalog 成员为准。）

- [ ] **Step 2: 对话框加区块**

`storagegovernancedialog.h/.cpp`：加「孤立受管文件」分组——`QTableWidget`（列：相对路径 / 大小 / 文件数）+「扫描孤立文件」`QPushButton`（objectName `orphanScanBtn`）+「删除选中…」+「在文件管理器中显示」。信号：

```cpp
  void orphanScanRequested();
  void orphanDeleteRequested(const QStringList &relPaths);
```

`setOrphans(const QVector<paleo::assetops::OrphanManagedDir> &)` 填表。删除动作走强确认（`PaleoNotify::ask`，文案「将永久删除孤立受管文件，不可恢复」）后经控制器执行（控制器加 `deleteOrphanDirs(relPaths)` 槽：逐目录 `QDir(abs).removeRecursively()`，`resolvedVersionPath` 同源容器守卫——路径必须落在 `artifacts/` 内，完成后重扫）。

- [ ] **Step 3: 接线 + 测试**

`datalist_undo.cpp` 治理对话框接线区补：

```cpp
  connect(dialog, &StorageGovernanceDialog::orphanScanRequested, controller, &StorageGovernanceController::scanOrphans);
  connect(controller, &StorageGovernanceController::orphansReady, dialog, &StorageGovernanceDialog::setOrphans);
  connect(dialog, &StorageGovernanceDialog::orphanDeleteRequested, controller, &StorageGovernanceController::deleteOrphanDirs);
```

跑：`cmake --build build -j8 && ctest --test-dir build -R 'storage|governance|tst_assetops' --output-on-failure -E 'core|perf'`
预期：PASS（正则按既有治理测试实际名调整）。

- [ ] **Step 4: 提交**

`git add src && git commit -m "feat(ui): 存储治理「孤立受管文件」——扫描/列表/删除/定位，承接索引级删除的副本去向"`

---

### Task 14: 文档同步与全量验证

**Files:**
- Modify: `docs/PROJECT_AREA_PLAN.md:57-77`（默认口径：默认索引外链，显式勾选受管复制）
- Modify: `docs/dataops/IMPORT.md`（存储方式与导入时命名）
- Modify: `docs/dataops/OPERATIONS.md:19-30`（操作矩阵加 renameAsset / removeFromCatalog）
- Modify: `docs/dataops/GAPS.md`（G-1.3 注记 UI 缺口已补——「从目录移除（保留文件）」）
- Modify: `docs/SECTIONS_ALIGNMENT_PLAN.md:31-37`（吸附/井序/重排行为）
- Modify: `TODOS.md`（无需新增；确认无相关递延项被遗忘即可）

- [ ] **Step 1: 文档更新（按上述锚点改写，与实现口径一致）**

- [ ] **Step 2: docs_drift 与 i18n 闸**

`ctest --test-dir build -R '^(docs_drift|i18n|ui_tokens|qgis_versions)$' --output-on-failure`
预期：PASS。

- [ ] **Step 3: 全量回归（linux/windows 车道口径）**

`ctest --test-dir build -j8 --output-on-failure -LE 'core|perf'`
预期：全绿；如有与本次口径调整相关的既有用例变红（例如钉旧导入默认的 UI 级用例），按 D1/D2/D4 新口径修正用例并在提交信息中说明。

- [ ] **Step 4: 提交**

`git add docs TODOS.md && git commit -m "docs: 索引式导入/对象命名/删索引保文件/剖面吸附——口径与操作矩阵同步"`

---

## Self-Review 记录（计划作者已完成）

- **规格覆盖**：D1→Task 3/4/5；对象命名→Task 1/2/3/5；D2→Task 6/7/8/9/10；D3（仅限井震）→Task 9 不改无地震体分支（按钮行为不变）；D4→Task 11/12/13。验收标准 1-6 各有任务与测试对应。
- **类型一致性**：`renameAsset`（Task 1 定义，Task 2 使用）；`ImportOptions.displayName`（Task 3 定义，Task 5 由 `PlannedItem.displayName` 喂入）；`ImportSession.linkExternal`（Task 4）；`orderWellsNearestNeighbor`/`nearestWellWithin`（Task 6 定义，Task 7/8 使用）；`spatiallyOrderedWells`/`wellSnapWells`（Task 7 定义，Task 9/10 使用）；`sectionPathCaptured(points, wellIds)` 与 `onSectionPathCaptured(points, wellIds)`（Task 8 定义，Task 9 连接）；`applyWellOrder`/`checkedWellIds`/`spatialReorderRequested`（Task 10）；`removeAssetsKeepFiles`/`scanOrphanManagedDirs`/`OrphanManagedDir`（Task 11 定义，Task 12/13 使用）。模式串 `"nearest"`/`"pca"` 三处一致。
- **已知风险点**（执行时验证）：`Well` 坐标有效性口径（Task 6/7 注明先看 `wellsection.h:105`）；`QgsSnappingConfig` 成员名以 vendored QGIS 4.2 头为准（Task 8 有回退说明）；`PaleoNotify::warn` 存在性（Task 12 注明）；`tst_catalog` 临时工程助手名（Task 1 注明沿用文件内既有模式）。
