// 层：视图
#include "folderconfirm.h"
#include "folderconfirm_internal.h"

#include "../../domain/projectclassifier.h" // 分类词表/固定辅助谓词（domain 纯函数）
#include "../paleotheme.h"

#include <memory>

#include <QComboBox>
#include <QCheckBox>
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
      {QStringLiteral("well_deviation"), QObject::tr("井斜")},
      {QStringLiteral("horizon"), QObject::tr("层位")},
      {QStringLiteral("seismic"), QObject::tr("地震")},
      {QStringLiteral("tabular"), QObject::tr("表格")},
      {QStringLiteral("geojson"), QObject::tr("GeoJSON")},
      {QStringLiteral("document"), QObject::tr("文档")},
      {QStringLiteral("image_reference"), QObject::tr("图像")},
      {QStringLiteral("single_factor_package"), QObject::tr("单因素数据包")},
      {QStringLiteral("outsource_workbook"), QObject::tr("外部成果工作簿")},
      {QStringLiteral("auxiliary"), QObject::tr("辅助资料")},
      {QStringLiteral("boundary"), QObject::tr("工区边界")},
      {QStringLiteral("facies_polygons"), QObject::tr("沉积相多边形")},
      {QStringLiteral("facies_schema"), QObject::tr("沉积相分类体系")},
      {QStringLiteral("paleogeographic_map"), QObject::tr("古地理图")},
      {QStringLiteral("seismic_prediction"), QObject::tr("地震相预测")},
      {QStringLiteral("tops"), QObject::tr("井分层")},
      {QStringLiteral("wells_prediction"), QObject::tr("测井相预测")},
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
    if (!row.skipped) {
      pathItem->setFlags(pathItem->flags() | Qt::ItemIsUserCheckable);
      pathItem->setCheckState(Qt::Checked);
    }
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
    const QString disp = row.displayType.isEmpty()
        ? folderRowDisplayType(row.path, row.classifiedType) : row.displayType;
    int idx = combo->findData(disp);
    if (idx < 0)
      idx = combo->findData(QStringLiteral("unknown"));
    if (idx >= 0)
      combo->setCurrentIndex(idx);

    // 锁定优先级：HZ28-6-1 固定参考（改不动）；跳过行同样禁改。
    const bool locked = isFixedAuxiliaryPath(row.path);
    if (row.skipped || locked || !row.typeEditable)
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

QString folderEstimateText(const QVector<FolderPreviewRow> &rows,
                           const QStringList &skippedPaths,
                           const QStringList &forceImportPaths)
{
  int willImport = 0, dupSkip = 0, enumSkip = 0, unknownSize = 0;
  int selectedSkip = 0;
  qint64 totalBytes = 0;
  for (const FolderPreviewRow &row : rows)
  {
    if (row.skipped)
    {
      ++enumSkip; // 软链逃逸/非普通文件（大小仍计入展示，但不导入）
      continue;
    }
    if (skippedPaths.contains(row.path))
    {
      ++selectedSkip;
      continue;
    }
    if (row.decision == QLatin1String("skip") && !forceImportPaths.contains(row.path))
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
  if (selectedSkip > 0)
    tails.append(QObject::tr("本次跳过 %1").arg(selectedSkip));
  if (!tails.isEmpty())
    text += QLatin1String("（") + tails.join(QStringLiteral("，")) + QLatin1String("）");
  return text;
}

bool folderImportCopyIntoProject(const QDialog *dlg)
{
  const auto *check =
      dlg ? dlg->findChild<QCheckBox *>(QStringLiteral("folderImportCopyCheck"))
          : nullptr;
  return check && check->isChecked();
}

void buildFolderConfirmDialog(QDialog *dlg, const QString &dir,
                              const QVector<FolderPreviewRow> &preview,
                              const Hooks &hooks)
{
  auto state = std::make_shared<detail::DialogState>();
  state->dialog = dlg;
  state->directory = dir;
  state->preview = preview;
  state->hooks = hooks;
  dlg->setObjectName(QStringLiteral("folderImportDialog"));
  dlg->setWindowTitle(QObject::tr("导入数据 — %1").arg(dir));
  dlg->resize(860, 520);
  auto *layout = new QVBoxLayout(dlg);
  state->hint = new QLabel(dlg);
  state->hint->setObjectName(QStringLiteral("folderImportHint"));
  state->hint->setWordWrap(true);
  layout->addWidget(state->hint);
  auto *crs = new QLabel(engineeringCrsSentence(), dlg);
  crs->setObjectName(QStringLiteral("folderCrsNote"));
  crs->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(crs, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(crs);
  state->table = new QTableWidget(0, 4, dlg);
  auto *table = state->table;
  table->setObjectName(QStringLiteral("folderTable"));
  table->setAccessibleName(QObject::tr("导入数据确认表"));
  table->setAccessibleDescription(hooks.importAllWithWells
      ? QObject::tr("显示识别类型、内容摘要与目标井；取消勾选可跳过该文件，确认后直接导入")
      : QObject::tr("显示识别类型、内容摘要与目标井；确认后直接导入"));
  table->setHorizontalHeaderLabels({QObject::tr("路径"), QObject::tr("类型"),
      QObject::tr("目标实体"), QObject::tr("内容 / 结果")});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setWordWrap(true);
  layout->addWidget(table, 1);
  populateFolderConfirmTable(table, dir, preview, &state->typeChoices);
  // 旧测试壳/嵌入方没有跳过出口；不展示无法兑现的选择。
  if (!hooks.importAllWithWells)
    for (int row = 0; row < preview.size(); ++row) {
      table->item(row, 0)->setData(Qt::CheckStateRole, QVariant());
      table->item(row, 0)->setFlags(table->item(row, 0)->flags() & ~Qt::ItemIsUserCheckable);
    }
  state->buildPreview(layout);
  auto *estimate = new QLabel(folderEstimateText(preview), dlg);
  state->estimate = estimate;
  estimate->setObjectName(QStringLiteral("folderEstimateLabel"));
  estimate->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(estimate, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  estimate->setVisible(!estimate->text().isEmpty());
  layout->addWidget(estimate);
  state->summary = new QLabel(dlg);
  state->summary->setObjectName(QStringLiteral("folderSummary"));
  state->summary->setWordWrap(true);
  state->summary->hide();
  layout->addWidget(state->summary);
  state->errors = new QLabel(dlg);
  state->errors->setObjectName(QStringLiteral("folderErrorReport"));
  state->errors->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(state->errors, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().errorText.name().toUpper());
  });
  state->errors->hide();
  layout->addWidget(state->errors);
  // 方向 D1：默认只建索引（不复制字节进工程）；勾选后才走旧受管复制。
  // 文案与单文件向导 singleImportCopyCheck 同款（两处同一口径）。
  state->copyCheck =
      new QCheckBox(QObject::tr("将文件复制进工程目录（受管副本）"), dlg);
  state->copyCheck->setObjectName(QStringLiteral("folderImportCopyCheck"));
  state->copyCheck->setToolTip(
      QObject::tr("默认只建立索引引用原文件，不复制；勾选后复制进工程 artifacts/raw"));
  layout->addWidget(state->copyCheck);
  // 与行跳过勾选同一纪律：壳没给 importAllWithWells 出口时勾选无人读取
  //（无法兑现），不展示。
  if (!hooks.importAllWithWells)
    state->copyCheck->hide();
  auto *buttons = new QDialogButtonBox(dlg);
  state->confirm = buttons->addButton(QObject::tr("确认导入"), QDialogButtonBox::AcceptRole);
  state->confirm->setObjectName(QStringLiteral("folderConfirmButton"));
  state->confirm->setDefault(true);
  state->cancel = buttons->addButton(QObject::tr("取消"), QDialogButtonBox::RejectRole);
  state->showUnresolved = buttons->addButton(QObject::tr("查看未决"), QDialogButtonBox::ActionRole);
  state->showUnresolved->setObjectName(QStringLiteral("folderShowUnresolvedButton"));
  state->showUnresolved->setAccessibleName(QObject::tr("查看未决资产"));
  state->showUnresolved->hide();
  QObject::connect(state->showUnresolved, &QAbstractButton::clicked, dlg, [state] {
    if (state->hooks.showUnresolved) state->hooks.showUnresolved();
    state->dialog->accept();
  });
  QObject::connect(state->confirm, &QAbstractButton::clicked, dlg, [state] { state->beginImport(); });
  QObject::connect(state->cancel, &QAbstractButton::clicked, dlg, &QDialog::reject);
  QObject::connect(dlg, &QDialog::rejected, dlg, [state] { state->cancelImport(); });
  QObject::connect(dlg, &QObject::destroyed, [state] { state->cancelImport(); });
  layout->addWidget(buttons);
  table->resizeRowsToContents();
  state->updateAvailability();
}

} // namespace PaleoFolderConfirm
