// 层：视图
#include "folderconfirm_internal.h"
#include "../../domain/projectclassifier.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

namespace PaleoFolderConfirm::detail {
namespace {
void showFailures(DialogState &state) {
  QStringList failures;
  for (const auto &row : state.results)
    if (row.outcome == FolderRowResult::Outcome::Failed)
      failures << QObject::tr("%1：%2").arg(QDir(state.directory).relativeFilePath(row.path),
          row.message.isEmpty() ? QObject::tr("导入失败，未写入工程；请检查文件后重试")
                                : row.message);
  QString text = failures.mid(0, 5).join(QLatin1Char('\n'));
  if (failures.size() > 5) text += QObject::tr("\n…等共 %1 项失败").arg(failures.size());
  state.errors->setText(text);
  state.errors->setVisible(!text.isEmpty());
}
QVariantMap importStats(const QVector<FolderRowResult> &rows) {
  int imported = 0, unresolved = 0, failed = 0, skipped = 0;
  for (const auto &row : rows)
    switch (row.outcome) {
    case FolderRowResult::Outcome::Imported: ++imported; break;
    case FolderRowResult::Outcome::Unresolved: ++unresolved; break;
    case FolderRowResult::Outcome::Failed: ++failed; break;
    case FolderRowResult::Outcome::Skipped: ++skipped; break;
    }
  return {{QStringLiteral("files"), rows.size()}, {QStringLiteral("imported"), imported},
      {QStringLiteral("unresolved"), unresolved}, {QStringLiteral("failed"), failed},
      {QStringLiteral("skipped"), skipped}};
}
}

QString pickPreviewFocusAsset(const QVector<FolderRowResult> &rows,
                              const QString &wellHeadAssetId) {
  if (!wellHeadAssetId.isEmpty()) return wellHeadAssetId;
  QString focus;
  for (const auto &row : rows) {
    if (row.assetId.isEmpty() ||
        (row.outcome != FolderRowResult::Outcome::Imported &&
         row.outcome != FolderRowResult::Outcome::Unresolved)) continue;
    if (QStringList{QStringLiteral("document"), QStringLiteral("image_reference"),
        QStringLiteral("reference"), QStringLiteral("unknown"),
        QStringLiteral("well_log")}.contains(row.classifiedType)) focus = row.assetId;
  }
  return focus;
}

void DialogState::cancelImport() {
  if (!importing || cancellationRequested) return;
  cancellationRequested = true;
  if (hooks.cancelImport) hooks.cancelImport();
}

void DialogState::beginImport() {
  if (importing || completed || !confirm->isEnabled()) return;
  const auto overrides = collectFolderTypeOverrides(table, preview, typeChoices);
  auto targets = wellTargets;
  QStringList skippedPaths;
  auto forced = forcePaths;
  for (int row = 0; row < preview.size(); ++row)
  {
    if (!preview[row].skipped && !rowSelected(row)) {
      skippedPaths << preview[row].path;
      forced.removeAll(preview[row].path);
    }
    if (!rowSelected(row) || typeChoices[row]->currentData().toString() != QLatin1String("well_log"))
      targets.remove(preview[row].path);
  }
  importing = true;
  cancellationRequested = false;
  table->setEnabled(false);
  confirm->setEnabled(false);
  // 「复制进工程」勾选随确认快照进 workflow（hooks 出口读取）——导入期间
  // 锁死，避免导入中再改造成「以为本批生效」的假意图。
  if (copyCheck) copyCheck->setEnabled(false);
  hint->setText(QObject::tr("正在导入；完成后逐项显示结果。"));
  for (auto *choice : typeChoices) choice->setEnabled(false);
  for (auto *choice : wellChoices) choice->setEnabled(false);
  const auto state = shared_from_this();
  const auto done = [state](const QVector<FolderRowResult> &rows, const QString &error) {
    if (state->dialog) state->applyResults(rows, error);
  };
  // F1（review-security）：预览展示过的路径全集随确认传入执行——执行期
  // 目录重扫多出的文件（预览后落盘）由 io 层一律跳过并提示重新预览。
  QStringList previewedPaths;
  previewedPaths.reserve(preview.size());
  for (const auto &row : preview) previewedPaths << row.path;
  if (hooks.importAllWithWells)
    hooks.importAllWithWells(overrides, forced, targets, expectedHashes, skippedPaths,
                             previewedPaths, done);
  else if (hooks.importAllWithForced) hooks.importAllWithForced(overrides, forcePaths, done);
  else if (hooks.importAll) hooks.importAll(overrides, done);
  else done({}, QObject::tr("导入服务未接线"));
}

void DialogState::applyResults(const QVector<FolderRowResult> &rows, const QString &error) {
  importing = false;
  table->setEnabled(true);
  if (rows.isEmpty() && !error.isEmpty()) {
    errors->setText(error);
    errors->show();
    if (copyCheck) copyCheck->setEnabled(true); // 整批失败：放开调整后可再确认
    for (int row = 0; row < typeChoices.size(); ++row)
      typeChoices[row]->setEnabled(preview[row].typeEditable && !preview[row].skipped &&
          !isFixedAuxiliaryPath(preview[row].path));
    for (auto *choice : wellChoices) choice->setEnabled(true);
    updateAvailability();
    return;
  }
  completed = true;
  for (int row = 0; row < preview.size(); ++row)
  {
    table->item(row, 0)->setFlags(table->item(row, 0)->flags() & ~Qt::ItemIsUserCheckable);
    // 结果页只开放失败行的类型/归属修正重试（重试不消费显示名覆盖）——
    // 「显示名」列同步锁死，不留「改了会生效」的假意图。
    if (QTableWidgetItem *nameItem = table->item(row, 4))
      nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
  }
  results.fill(FolderRowResult{}, preview.size());
  QString wellHead;
  bool unresolved = false;
  QStringList unreviewed; // 预览后新出现、被 io 层跳过的文件（无预览行可对应）
  const auto state = shared_from_this();
  for (const auto &result : rows) {
    int row = -1;
    for (int index = 0; index < preview.size(); ++index)
      if (preview[index].path == result.path) { row = index; break; }
    if (row < 0)
    {
      if (result.outcome == FolderRowResult::Outcome::Skipped)
        unreviewed << QStringLiteral("%1：%2").arg(
            QFileInfo(result.path).fileName(), result.message);
      continue;
    }
    results[row] = result;
    const bool failed = result.outcome == FolderRowResult::Outcome::Failed;
    typeChoices[row]->setEnabled(failed && preview[row].typeEditable &&
        !preview[row].skipped && !isFixedAuxiliaryPath(preview[row].path));
    if (wellChoices.contains(row)) wellChoices[row]->setEnabled(failed);
    writeFolderRowResult(table, row, result, [state](int index) { state->retryRow(index); });
    if (result.outcome == FolderRowResult::Outcome::Imported &&
        result.classifiedType == QLatin1String("well_head") && wellHead.isEmpty() &&
        hooks.importedWellHead) wellHead = hooks.importedWellHead(result.path);
    unresolved |= result.outcome == FolderRowResult::Outcome::Unresolved;
  }
  summary->setText(folderImportSummaryText(results));
  summary->show();
  showFailures(*this);
  showUnresolved->setVisible(unresolved);
  if (!unreviewed.isEmpty())
  {
    // 与 showFailures 共用 errors 区：追加而非覆盖（失败清单可能已在里面）。
    const QString note = QObject::tr("以下 %1 个文件在预览后才出现，未随本批导入；"
                                     "如需导入请重新打开导入预览：\n%2")
                             .arg(unreviewed.size())
                             .arg(unreviewed.join(QLatin1Char('\n')));
    const QString existing = errors->text();
    errors->setText(existing.isEmpty() ? note : existing + QLatin1Char('\n') + note);
    errors->show();
  }
  cancel->setText(QObject::tr("关闭"));
  hint->setText(QObject::tr("导入完成；失败原因在原行显示，可修正后重试。"));
  const QString focus = pickPreviewFocusAsset(rows, wellHead);
  if (hooks.previewAsset && !focus.isEmpty()) hooks.previewAsset(focus);
  if (hooks.stampSourceArea) hooks.stampSourceArea(importStats(rows));
}

void DialogState::retryRow(int row) {
  if (row < 0 || row >= results.size()) return;
  QString force;
  const QString type = typeChoices[row]->currentData().toString();
  if (typeChoices[row]->isEnabled() && isClassifierType(type) &&
      type != preview[row].classifiedType) force = type;
  const QString target = type == QLatin1String("well_log")
      ? wellTargets.value(preview[row].path) : QString();
  if (type == QLatin1String("well_log") && wellChoices.contains(row) && target.isEmpty()) {
    errors->setText(QObject::tr("请先为该测井文件选择目标井，再重试。"));
    errors->show();
    return;
  }
  FolderRowResult result;
  if (hooks.importRowWithWell)
    result = hooks.importRowWithWell(preview[row].path, force, target,
                                     expectedHashes.value(preview[row].path));
  else if (hooks.importRow) result = hooks.importRow(preview[row].path, force);
  else {
    result.outcome = FolderRowResult::Outcome::Failed;
    result.message = QObject::tr("导入服务未接线");
  }
  results[row] = result;
  const auto state = shared_from_this();
  writeFolderRowResult(table, row, result, [state](int index) { state->retryRow(index); });
  if (result.outcome != FolderRowResult::Outcome::Failed) {
    typeChoices[row]->setEnabled(false);
    if (wellChoices.contains(row)) wellChoices[row]->setEnabled(false);
  }
  summary->setText(folderImportSummaryText(results));
  showFailures(*this);
  // UX（review-ux）：重试可能改变未决行集合——「查看未决」随最新结果刷新。
  bool anyUnresolved = false;
  for (const auto &r : results)
    anyUnresolved |= r.outcome == FolderRowResult::Outcome::Unresolved;
  showUnresolved->setVisible(anyUnresolved);
}
} // namespace PaleoFolderConfirm::detail
