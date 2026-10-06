// 层：视图
#include "folderconfirm.h"

#include "../../domain/projectclassifier.h" // 分类词表/固定辅助谓词（domain 纯函数）
#include "../paleotheme.h"

#include <memory>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include "../notifications/paleonotify.h"
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace PaleoFolderConfirm
{

QString folderTypeLabel(const QString &type)
{
  // 用户可见类型名：中文源串走 tr（函数内静态表，首次调用时才求值，
  // 此时翻译器早已装好）。
  static const QHash<QString, QString> kLabels = {
      {QStringLiteral("well_head"), QObject::tr("井口")},
      {QStringLiteral("well_log"), QObject::tr("测井")},
      {QStringLiteral("well_stratification"), QObject::tr("井分层")},
      {QStringLiteral("time_depth"), QObject::tr("时深")},
      {QStringLiteral("horizon"), QObject::tr("层位")},
      {QStringLiteral("seismic"), QObject::tr("地震")},
      {QStringLiteral("tabular"), QObject::tr("表格")},
      {QStringLiteral("geojson"), QObject::tr("GeoJSON")},
      {QStringLiteral("document"), QObject::tr("文档")},
      {QStringLiteral("image_reference"), QObject::tr("图像")},
      {QStringLiteral("reference"), QObject::tr("参考资料")},
      {QStringLiteral("unknown"), QObject::tr("未知")}};
  return kLabels.value(type, type); // 词表外类型裸显 id（type 仍存 item data）
}

QString folderRowDisplayType(const QString &path, const QString &classifiedType)
{
  if (isFixedAuxiliaryPath(path))
    return QStringLiteral("reference"); // HZ28-6-1：固定参考（下拉同时锁死）
  if (isDefaultReferencePath(path))
  {
    // 「参考资料」目录内，分类到井类/未判内容的行默认显示「参考」——确认不改
    // 也作为 override=reference 送达后端，保持阶段 D 语义。document/
    // image_reference/geojson/seismic/horizon 等显示真实类型：它们本来就走
    // 辅助实体，且类型名驱动预览分支（document → PDF 预览）。
    static const QSet<QString> kWellish = {QStringLiteral("well_head"),
                                           QStringLiteral("well_log"),
                                           QStringLiteral("unknown")};
    if (kWellish.contains(classifiedType))
      return QStringLiteral("reference");
  }
  return classifiedType.isEmpty() ? QStringLiteral("unknown") : classifiedType;
}

QString engineeringCrsSentence()
{
  // T22/§3 契约句：工区导入统一展示的 CRS 说明（文件夹确认表 + 单文件
  // 导入确认都只读挂这句）。状态栏短句另行，与 PDF 页脚同一文案。
  return QObject::tr(
      "局部工程坐标，单位米。源文件里的 EPSG:4326 只是标签，不会画到地图上。");
}

void populateFolderConfirmTable(QTableWidget *table, const QString &rootDir,
                                const QVector<FolderPreviewRow> &rows,
                                QVector<QComboBox *> *combosOut)
{
  const QDir root(rootDir);
  const QStringList vocab = projectClassifierTypes();
  table->setRowCount(0);
  if (combosOut)
  {
    combosOut->clear();
    combosOut->reserve(rows.size());
  }
  for (const FolderPreviewRow &row : rows)
  {
    const int r = table->rowCount();
    table->insertRow(r);
    auto *pathItem = new QTableWidgetItem(root.relativeFilePath(row.path));
    pathItem->setToolTip(row.path);
    pathItem->setData(Qt::UserRole, row.path);
    table->setItem(r, 0, pathItem);
    table->setItem(r, 1, new QTableWidgetItem);
    table->setItem(r, 2, new QTableWidgetItem);
    table->setItem(r, 3, new QTableWidgetItem);

    auto *combo = new QComboBox(table);
    combo->setObjectName(QStringLiteral("folderType%1").arg(r));
    for (const QString &t : vocab)
      combo->addItem(folderTypeLabel(t), t); // type 存 data，不靠文本反推
    // 分类器给了词表外类型（未来扩展）→ 追加一项保住真实类型可选。
    if (!row.classifiedType.isEmpty() && !vocab.contains(row.classifiedType))
      combo->addItem(row.classifiedType, row.classifiedType);
    const QString disp = folderRowDisplayType(row.path, row.classifiedType);
    int idx = combo->findData(disp);
    if (idx < 0)
      idx = combo->findData(QStringLiteral("unknown"));
    if (idx >= 0)
      combo->setCurrentIndex(idx);

    // 锁定优先级：HZ28-6-1 固定参考（改不动）；跳过行同样禁改。
    const bool locked = isFixedAuxiliaryPath(row.path);
    if (row.skipped || locked)
      combo->setEnabled(false);
    if (locked)
    {
      combo->setToolTip(QObject::tr("该文件固定为参考资料"));
      pathItem->setToolTip(QObject::tr("%1\n该文件固定为参考资料").arg(row.path));
    }
    table->setCellWidget(r, 1, combo);
    if (combosOut)
      combosOut->append(combo);
    if (row.skipped)
    {
      table->item(r, 3)->setText(QObject::tr("跳过：%1").arg(row.skipReason));
      for (int c = 0; c < 4; ++c)
        table->item(r, c)->setFlags(table->item(r, c)->flags() & ~Qt::ItemIsEnabled);
    }
    else
    {
      // 方向 30：归位预览——plan 期身份匹配结论预显在「实体」列（导入完成后
      // writeFolderRowResult 用行结果的实体名覆盖）。
      if (!row.entityPreview.isEmpty())
        table->item(r, 2)->setText(row.entityPreview);
      // C 包 IngestPlan：plan 期决策逐行可见——重复→跳过 / 重复→新版本；
      // 未决行不在此预写（保持既有口径：结果列导入后才写「未决」）。
      const QString decisionText =
          row.decision == QLatin1String("skip")
              ? QObject::tr("重复→跳过")
              : row.decision == QLatin1String("as_new_version")
                    ? QObject::tr("重复→新版本")
                    : QString();
      if (!decisionText.isEmpty())
        table->item(r, 3)->setText(decisionText);
    }
  }
}

QMap<QString, QString>
collectFolderTypeOverrides(const QTableWidget *table,
                           const QVector<FolderPreviewRow> &rows,
                           const QVector<QComboBox *> &combos)
{
  QMap<QString, QString> overrides;
  for (int r = 0; r < combos.size() && r < rows.size(); ++r)
  {
    if (!combos[r] || !combos[r]->isEnabled())
      continue; // 跳过行/锁定行不参与导入，改动也不成 override
    const QString t = combos[r]->currentData().toString(); // item data，非显示文本
    const QString path = table->item(r, 0)->data(Qt::UserRole).toString();
    // 只在「合法类型」且「不同于分类器原类型」时发 override——这样不动
    // 下拉/保持默认的行不发出多余覆盖（参考资料默认「参考」属有意覆盖）。
    if (isClassifierType(t) && t != rows.at(r).classifiedType)
      overrides.insert(path, t);
  }
  return overrides;
}

void writeFolderRowResult(QTableWidget *table, int row, const FolderRowResult &res,
                          const std::function<void(int)> &onRetry)
{
  using Outcome = FolderRowResult::Outcome;
  QString outcomeText;
  switch (res.outcome)
  {
  case Outcome::Imported:
    outcomeText = QObject::tr("已入库");
    break;
  case Outcome::Unresolved:
    outcomeText = QObject::tr("未决");
    break;
  case Outcome::Failed:
    outcomeText = QObject::tr("失败");
    break;
  case Outcome::Skipped:
    outcomeText = QObject::tr("跳过");
    break;
  }
  const QString text = res.message.isEmpty()
                           ? outcomeText
                           : QObject::tr("%1：%2").arg(outcomeText, res.message);
  table->item(row, 2)->setText(res.entityName);
  // 清掉旧的重试控件——removeCellWidget 只摘不删，控件会活成表内孤儿。
  if (QWidget *old = table->cellWidget(row, 3))
  {
    table->removeCellWidget(row, 3);
    old->setParent(nullptr);
    old->deleteLater();
  }
  table->item(row, 3)->setText(text);
  if (res.outcome == Outcome::Failed && onRetry)
  {
    // 失败行的「重试」：按当前下拉类型只重导这一行。
    auto *cell = new QWidget(table);
    auto *hl = new QHBoxLayout(cell);
    hl->setContentsMargins(PaleoTheme::tokens().spacingXs, 0, PaleoTheme::tokens().spacingXs, 0);
    auto *msg = new QLabel(text, cell);
    msg->setWordWrap(true);
    auto *retry = new QPushButton(QObject::tr("重试"), cell);
    retry->setObjectName(QStringLiteral("folderRetry"));
    retry->setAccessibleName(
        QObject::tr("重试导入 %1").arg(table->item(row, 0)->text()));
    hl->addWidget(msg, 1);
    hl->addWidget(retry, 0);
    QObject::connect(retry, &QPushButton::clicked, table,
                     [onRetry, row] { onRetry(row); });
    table->setCellWidget(row, 3, cell);
  }
}

QString folderImportSummaryText(const QVector<FolderRowResult> &rows)
{
  using Outcome = FolderRowResult::Outcome;
  int imported = 0, unresolved = 0, failed = 0, skipped = 0;
  for (const auto &res : rows)
    switch (res.outcome)
    {
    case Outcome::Imported:
      ++imported;
      break;
    case Outcome::Unresolved:
      ++unresolved;
      break;
    case Outcome::Failed:
      ++failed;
      break;
    case Outcome::Skipped:
      ++skipped;
      break;
    }
  QString text = QObject::tr("入库 %1，未决 %2，失败 %3")
                     .arg(imported)
                     .arg(unresolved)
                     .arg(failed);
  if (skipped > 0) // D3：「跳过」保留为第四计数（符号链接/非普通文件如实报）
    text += QObject::tr("，跳过 %1").arg(skipped);
  return text;
}

QString folderEstimateText(const QVector<FolderPreviewRow> &rows)
{
  int willImport = 0, dupSkip = 0, enumSkip = 0, unknownSize = 0;
  qint64 totalBytes = 0;
  for (const FolderPreviewRow &row : rows)
  {
    if (row.skipped)
    {
      ++enumSkip; // 软链逃逸/非普通文件（大小仍计入展示，但不导入）
      continue;
    }
    if (row.decision == QLatin1String("skip"))
    {
      ++dupSkip; // plan 期 sha 重复 → 默认跳过（可「仍导入」）
      continue;
    }
    ++willImport;
    if (row.sizeBytes >= 0)
      totalBytes += row.sizeBytes;
    else
      ++unknownSize;
  }
  if (rows.isEmpty())
    return QString();
  // 人类可读大小：B/KB/MB/GB 一位小数（>200MB 的 SEG-Y 常态走 MB/GB 位）。
  const auto humanSize = [](qint64 b) -> QString {
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"),
                                   QStringLiteral("MB"), QStringLiteral("GB")};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u + 1 < units.size())
    {
      v /= 1024.0;
      ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v >= 10 || u == 0 ? 0 : 1).arg(units.at(u));
  };
  QString text = QObject::tr("将导入 %1 项 · 约 %2").arg(willImport).arg(humanSize(totalBytes));
  if (unknownSize > 0)
    text += QObject::tr("（%1 项大小未知）").arg(unknownSize);
  QStringList tails;
  if (dupSkip > 0)
    tails.append(QObject::tr("重复跳过 %1").arg(dupSkip));
  if (enumSkip > 0)
    tails.append(QObject::tr("枚举跳过 %1").arg(enumSkip));
  if (!tails.isEmpty())
    text += QLatin1String("（") + tails.join(QStringLiteral("，")) + QLatin1String("）");
  return text;
}

void buildFolderConfirmDialog(QDialog *dlg, const QString &dir,
                              const QVector<FolderPreviewRow> &preview,
                              const Hooks &hooks)
{
  dlg->setObjectName(QStringLiteral("folderImportDialog"));
  dlg->setWindowTitle(QObject::tr("导入工区文件夹 — %1").arg(dir));
  dlg->resize(760, 420);
  auto *lay = new QVBoxLayout(dlg);
  auto *hint = new QLabel(
      QObject::tr("确认每个文件的类型（可改）后导入；井口文件会先入库。"), dlg);
  hint->setWordWrap(true);
  lay->addWidget(hint);
  // T22：CRS 契约句——只读一行，挂在确认表上方。
  auto *crsNote = new QLabel(engineeringCrsSentence(), dlg);
  crsNote->setObjectName(QStringLiteral("folderCrsNote"));
  crsNote->setWordWrap(true);
  // text-muted 活体（对话框生命周期内跟随主题切换）。
  PaleoTheme::applyThemedStyleSheet(
      crsNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  lay->addWidget(crsNote);

  auto *table = new QTableWidget(0, 4, dlg);
  table->setObjectName(QStringLiteral("folderTable"));
  // T32 a11y：文件夹确认表报名 + 说明（每行可改类型、锁死行只读）。
  table->setAccessibleName(QObject::tr("文件夹导入确认表"));
  table->setAccessibleDescription(QObject::tr(
      "列出所选文件夹里的每个文件：确认或修改类型后导入，井口文件先入库"));
  table->setHorizontalHeaderLabels(
      {QObject::tr("路径"), QObject::tr("类型"), QObject::tr("实体"),
       QObject::tr("结果")});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  lay->addWidget(table);

  QVector<QComboBox *> combos;
  populateFolderConfirmTable(table, dir, preview, &combos);

  // T2 大小估算：预览期一行估算（将导入多少/多大/跳过多少）+ 每行大小进
  // 路径 tooltip——不动四列结构（既有测试与结果列控件挂点不迁移）。
  for (int r = 0; r < preview.size() && r < table->rowCount(); ++r)
  {
    if (preview.at(r).sizeBytes < 0)
      continue;
    QTableWidgetItem *pathItem = table->item(r, 0);
    if (!pathItem)
      continue;
    const qint64 b = preview.at(r).sizeBytes;
    const QString human = b >= 1024 * 1024 * 1024
                              ? QStringLiteral("%1 GB").arg(b / 1073741824.0, 0, 'f', 1)
                              : b >= 1024 * 1024
                                    ? QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1)
                                    : b >= 1024
                                          ? QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1)
                                          : QStringLiteral("%1 B").arg(b);
    pathItem->setToolTip(QObject::tr("%1\n大小 %2").arg(pathItem->toolTip(), human));
  }
  auto *estimate = new QLabel(dlg);
  estimate->setObjectName(QStringLiteral("folderEstimateLabel"));
  estimate->setText(folderEstimateText(preview));
  PaleoTheme::applyThemedStyleSheet(
      estimate, [] { return PaleoTheme::mutedCaptionStyleSheet(); }); // text-muted 活体
  if (estimate->text().isEmpty())
    estimate->hide();
  lay->addWidget(estimate);

  // T2 跳过策略：「重复→跳过」行的「仍导入」按钮（结果列挂点，与「重试」
  // 同一模式）。只有壳给了 importAllWithForced 才出现——旧壳零扰动。
  auto forcePaths = std::make_shared<QStringList>();
  if (hooks.importAllWithForced)
    for (int r = 0; r < preview.size() && r < table->rowCount(); ++r)
    {
      const FolderPreviewRow &row = preview.at(r);
      if (row.skipped || row.decision != QLatin1String("skip"))
        continue;
      auto *btn = new QPushButton(QObject::tr("仍导入"), table);
      btn->setObjectName(QStringLiteral("folderForceImport%1").arg(r));
      btn->setAccessibleName(
          QObject::tr("仍导入 %1").arg(table->item(r, 0)->text()));
      const QString path = row.path;
      QObject::connect(btn, &QPushButton::clicked, table,
                       [btn, forcePaths, path]() {
                         if (forcePaths->contains(path))
                           return;
                         forcePaths->append(path);
                         btn->setText(QObject::tr("将导入"));
                         btn->setEnabled(false);
                       });
      table->setCellWidget(r, 3, btn);
    }

  auto *summary = new QLabel(dlg);
  summary->setObjectName(QStringLiteral("folderSummary"));
  summary->setWordWrap(true);
  summary->hide();
  lay->addWidget(summary);
  // T2 错误行报告：失败行明细（路径: 原因，至多 5 行 + 「等 n 项」）——
  // 汇总计数之外把「为什么失败」留在对话里，用户不必逐行扫表。
  auto *errorReport = new QLabel(dlg);
  errorReport->setObjectName(QStringLiteral("folderErrorReport"));
  errorReport->setWordWrap(true);
  // error token（语义红——失败明细是状态不是装饰；活体随主题）。
  PaleoTheme::applyThemedStyleSheet(errorReport, [] {
    return QStringLiteral("color: %1;")
        .arg(PaleoTheme::tokens().errorText.name().toUpper());
  });
  errorReport->hide();
  lay->addWidget(errorReport);

  auto *buttons = new QDialogButtonBox(dlg);
  auto *confirm =
      buttons->addButton(QObject::tr("确认导入"), QDialogButtonBox::AcceptRole);
  confirm->setObjectName(QStringLiteral("folderConfirmButton"));
  auto *cancel = buttons->addButton(QObject::tr("取消"), QDialogButtonBox::RejectRole);
  // T31「查看未决」：导入完成后出现——把数据页资产表过滤到未决行，直接
  // 指向「挂到这口井」的挂接入口（不留「导完了然后呢」的断头路）。
  auto *showUnresolved =
      buttons->addButton(QObject::tr("查看未决"), QDialogButtonBox::ActionRole);
  showUnresolved->setObjectName(QStringLiteral("folderShowUnresolvedButton"));
  showUnresolved->setVisible(false);
  showUnresolved->setAccessibleName(QObject::tr("查看未决资产"));
  QObject::connect(showUnresolved, &QAbstractButton::clicked, dlg, [dlg, hooks]() {
    if (hooks.showUnresolved)
      hooks.showUnresolved();
    dlg->accept();
  });
  QObject::connect(cancel, &QAbstractButton::clicked, dlg, &QDialog::reject);

  // 对话框 exec 在 build 返回之后——行结果/重试回调的生存期挂到 shared 状态，
  // 不捕局部引用。
  auto results = std::make_shared<QVector<FolderRowResult>>();
  auto retryFn = std::make_shared<std::function<void(int)>>();
  const std::function<void(int)> retryCb =
      [retryFn](int r) { if (*retryFn) (*retryFn)(r); };

  // 行重试：只重导这一行的文件，类型取当前下拉值（合法且不同于分类器原类
  // 型才成 override；锁定/灰显行不带覆盖）。
  *retryFn = [retryFn, retryCb, table, summary, combos, preview, results,
              hooks](int r) {
    if (r < 0 || r >= preview.size() || r >= results->size())
      return;
    const QString path = table->item(r, 0)->data(Qt::UserRole).toString();
    QString force;
    if (combos.value(r) && combos[r]->isEnabled())
    {
      const QString t = combos[r]->currentData().toString();
      if (isClassifierType(t) && t != preview.at(r).classifiedType)
        force = t;
    }
    FolderRowResult rowRes;
    QString rerr;
    if (hooks.importRow)
      rowRes = hooks.importRow(path, force);
    else
    {
      rowRes.outcome = FolderRowResult::Outcome::Failed;
      rowRes.message = QStringLiteral("导入未接线");
    }
    (void)rerr;
    (*results)[r] = rowRes;
    writeFolderRowResult(table, r, rowRes, retryCb); // 仍失败 → 重试按钮回挂
    // 行不再是失败：收掉改类型入口；仍失败的保留下拉（可换类型再试）。
    if (rowRes.outcome != FolderRowResult::Outcome::Failed && combos.value(r))
      combos[r]->setEnabled(false);
    summary->setText(folderImportSummaryText(*results));
  };

  QObject::connect(confirm, &QAbstractButton::clicked, dlg,
                   [dlg, dir, table, summary, errorReport, confirm, cancel,
                    showUnresolved, combos, preview, results, retryCb, hooks,
                    forcePaths]() {
    const QMap<QString, QString> overrides =
        collectFolderTypeOverrides(table, preview, combos);
    confirm->setEnabled(false); // 确认只走一遍（异步在途也一样）

    // 导入结果回表——同步路径与任务终态共用（在 GUI 线程执行）。
    const auto applyResults =
        [dlg, dir, table, summary, errorReport, confirm, cancel,
         showUnresolved, combos, preview, results, retryCb,
         hooks](const QVector<FolderRowResult> &res, const QString &importErr) {
      if (res.isEmpty() && !importErr.isEmpty())
      {
        PaleoNotify::warning(dlg, QObject::tr("导入工区文件夹"), importErr);
        confirm->setEnabled(true); // 整体失败可重试
        return;
      }
      // D5：结果序按生效类型两阶段排——改过类型的行可能换阶段，按「路径」
      // 回行而不是按索引；results 与表行同序存放，供重试回写与汇总重算。
      results->fill(FolderRowResult{}, preview.size());
      using Outcome = FolderRowResult::Outcome;
      QString wellHeadAssetId;
      for (const FolderRowResult &rowRes : res)
      {
        int r = -1;
        for (int i = 0; i < preview.size(); ++i)
          if (preview.at(i).path == rowRes.path)
          {
            r = i;
            break;
          }
        if (r < 0)
          continue;
        (*results)[r] = rowRes;
        writeFolderRowResult(table, r, rowRes, retryCb);
        if (rowRes.outcome == Outcome::Imported &&
            rowRes.classifiedType == QLatin1String("well_head") &&
            wellHeadAssetId.isEmpty() && hooks.importedWellHead)
        {
          // 找回刚入库的井口资产：按文件名反查（壳/workflow 侧 catalog 读）。
          wellHeadAssetId = hooks.importedWellHead(rowRes.path);
        }
      }
      summary->setText(folderImportSummaryText(*results));
      summary->show();
      // T2 错误行报告：失败行明细（至多 5 行 + 余量计数）；无失败则隐藏。
      QStringList failures;
      for (const FolderRowResult &rowRes : res)
        if (rowRes.outcome == FolderRowResult::Outcome::Failed)
          failures.append(
              QObject::tr("%1：%2")
                  .arg(QDir::isAbsolutePath(rowRes.path)
                           ? QDir(dir).relativeFilePath(rowRes.path)
                           : rowRes.path,
                       rowRes.message.isEmpty() ? QObject::tr("导入失败")
                                                : rowRes.message));
      if (!failures.isEmpty())
      {
        const int shown = qMin(5, failures.size());
        QString text = failures.mid(0, shown).join(QLatin1Char('\n'));
        if (failures.size() > shown)
          text += QObject::tr("\n…等共 %1 项失败").arg(failures.size());
        errorReport->setText(text);
        errorReport->show();
      }
      else
        errorReport->hide();
      // 有未决行才露「查看未决」入口（T31）。
      bool anyUnresolved = false;
      for (const auto &rowRes : *results)
        if (rowRes.outcome == Outcome::Unresolved)
          anyUnresolved = true;
      showUnresolved->setVisible(anyUnresolved);
      // 结果留在表里给用户过目；仍失败的行保留下拉（可换类型再点「重试」），
      // 其余行锁定。
      for (int r = 0; r < combos.size(); ++r)
        if (r >= results->size() || results->at(r).outcome != Outcome::Failed)
          combos[r]->setEnabled(false);
      cancel->setText(QObject::tr("关闭"));
      if (hooks.previewAsset && !wellHeadAssetId.isEmpty())
        hooks.previewAsset(wellHeadAssetId);
      // PROJECT_FILE_DESIGN：「从工区文件夹新建」的工程把本次导入统计写回
      // project.paleo.sourceArea（目录不匹配时 stampSourceArea 自拒，不污染
      // 普通导入路径下的工程）。
      if (hooks.stampSourceArea)
      {
        int nImported = 0, nUnresolved = 0, nFailed = 0, nSkipped = 0;
        for (const auto &rr : res)
          switch (rr.outcome)
          {
          case Outcome::Imported: ++nImported; break;
          case Outcome::Unresolved: ++nUnresolved; break;
          case Outcome::Failed: ++nFailed; break;
          case Outcome::Skipped: ++nSkipped; break;
          }
        hooks.stampSourceArea(QVariantMap{
            {QStringLiteral("files"), res.size()},
            {QStringLiteral("imported"), nImported},
            {QStringLiteral("unresolved"), nUnresolved},
            {QStringLiteral("failed"), nFailed},
            {QStringLiteral("skipped"), nSkipped}});
      }
    };

    // 对话框中途关闭 → 结果弃置（catalog 状态已入库，可重开表看）。
    QPointer<QDialog> guard(dlg);
    const auto guardedApply = [guard, applyResults](const QVector<FolderRowResult> &r,
                                                    const QString &e) {
      if (guard)
        applyResults(r, e);
    };
    // T2 跳过策略：壳给了「仍导入」通道就带上改判集合；否则旧口径零扰动。
    if (hooks.importAllWithForced)
      hooks.importAllWithForced(overrides, *forcePaths, guardedApply);
    else if (hooks.importAll)
      hooks.importAll(overrides, guardedApply);
  });
  lay->addWidget(buttons);
}

} // namespace PaleoFolderConfirm
