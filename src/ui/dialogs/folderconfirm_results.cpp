// 层：视图
#include "folderconfirm_internal.h"
#include "../../domain/projectclassifier.h"
#include <QComboBox>
#include <QDialog>
#include <QDir>
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
          row.message.isEmpty() ? QObject::tr("导入失败") : row.message);
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
  hint->setText(QObject::tr("正在导入；完成后逐项显示结果。"));
  for (auto *choice : typeChoices) choice->setEnabled(false);
  for (auto *choice : wellChoices) choice->setEnabled(false);
  const auto state = shared_from_this();
  const auto done = [state](const QVector<FolderRowResult> &rows, const QString &error) {
    if (state->dialog) state->applyResults(rows, error);
  };
  if (hooks.importAllWithWells)
    hooks.importAllWithWells(overrides, forced, targets, expectedHashes, skippedPaths, done);
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
    for (int row = 0; row < typeChoices.size(); ++row)
      typeChoices[row]->setEnabled(preview[row].typeEditable && !preview[row].skipped &&
          !isFixedAuxiliaryPath(preview[row].path));
    for (auto *choice : wellChoices) choice->setEnabled(true);
    updateAvailability();
    return;
  }
  completed = true;
  for (int row = 0; row < preview.size(); ++row)
    table->item(row, 0)->setFlags(table->item(row, 0)->flags() & ~Qt::ItemIsUserCheckable);
  results.fill(FolderRowResult{}, preview.size());
  QString wellHead;
  bool unresolved = false;
  const auto state = shared_from_this();
  for (const auto &result : rows) {
    int row = -1;
    for (int index = 0; index < preview.size(); ++index)
      if (preview[index].path == result.path) { row = index; break; }
    if (row < 0) continue;
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
}
} // namespace PaleoFolderConfirm::detail
