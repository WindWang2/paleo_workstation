// 层：视图
#pragma once
#include "folderconfirm.h"
#include <QPointer>
#include <memory>

class QLabel;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace PaleoFolderConfirm::detail {
// 一次确认窗口的值状态。只收集用户选择与显示结果；解析、归属与入库由 hooks 执行。
struct DialogState : std::enable_shared_from_this<DialogState> {
  QPointer<QDialog> dialog;
  QString directory;
  QVector<FolderPreviewRow> preview;
  Hooks hooks;
  QTableWidget *table = nullptr;
  QLabel *hint = nullptr;
  QLabel *estimate = nullptr;
  QLabel *summary = nullptr;
  QLabel *errors = nullptr;
  QPlainTextEdit *dataPreview = nullptr;
  QPushButton *confirm = nullptr;
  QPushButton *cancel = nullptr;
  QPushButton *showUnresolved = nullptr;
  // 方向 D1：「复制进工程」勾选（确认按钮区上方，默认不勾选 = 只建索引）。
  // 确认动作在对话框内经 hooks 发起——壳在 importAllWithWells 出口里读它
  //（folderImportCopyIntoProject）调 FolderImportWorkflow::setLinkExternal。
  QCheckBox *copyCheck = nullptr;
  QVector<QComboBox *> typeChoices;
  QMap<int, QComboBox *> wellChoices;
  QMap<QString, QString> wellTargets;
  QMap<QString, QString> expectedHashes;
  QStringList forcePaths;
  QVector<FolderRowResult> results;
  bool importing = false;
  bool completed = false;
  bool cancellationRequested = false;

  void buildPreview(QVBoxLayout *layout);
  bool rowSelected(int row) const;
  void updateAvailability();
  void showDataPreview(int row);
  void beginImport();
  void cancelImport();
  void applyResults(const QVector<FolderRowResult> &rows, const QString &error);
  void retryRow(int row);
};
void addWellChoice(const std::shared_ptr<DialogState> &state, int row);
QString pickPreviewFocusAsset(const QVector<FolderRowResult> &rows,
                              const QString &wellHeadAssetId);
} // namespace PaleoFolderConfirm::detail
