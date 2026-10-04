// 层：视图
#pragma once
#include <QDialog>
#include <QString>
#include <QVector>

#include "../../domain/wellrecords.h"
#include "../../domain/welltopsedit.h"

#include <memory>

class DataCatalog;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QToolButton;
class WellTopsEditorWorkflow;

// ui/welltops — 方向 32 分层表编辑器（按井打开 DC.dat 分层）。
// 视图纪律：表格/校验面板/批量/版本菜单只发意图，落库一律经
// WellTopsEditorWorkflow（GUI 线程单事务）；本类不直接写 catalog。
// 编辑保存 = 同资产新 DERIVED 版本；行级 CRUD、脏行标记、校验错误阻断
// 保存（警告放行）；批量/回滚立即单事务提交后整表重载。
class WellTopsEditorDialog : public QDialog
{
  Q_OBJECT
public:
  WellTopsEditorDialog(DataCatalog *catalog, const QString &projectDir, const QString &assetId,
                       QWidget *parent = nullptr);
  ~WellTopsEditorDialog() override;

private slots:
  void onWellChanged();
  void onInsertRow();
  void onDeleteRow();
  void onMoveRow(int delta);
  void onSortByDepth();
  void onValidate();
  void onSave();
  void onMergeFromFile();
  void onBatchRename();
  void onBatchShift();
  void onBatchDelete();
  void onRollbackMenu(QAction *action);

private:
  // 表列（0 基）：状态/层名/MD/TVD/X/Y/Z/Time(ms)。
  // Z 与 X/Y 同列组（解析器 t.size()>=6 才读、无独立判空标志）——不展示即
  // 丢值（评审 H1），必须进表。
  enum Col
  {
    ColStatus = 0,
    ColTopName,
    ColMd,
    ColTvd,
    ColX,
    ColY,
    ColZ,
    ColTime,
    ColCount
  };

  void buildUi();
  void reloadAll();
  void fillTableFrom(const QVector<WellTopRecord> &rows);
  void refreshRowStatus(int row);
  QVector<QStringList> tableTexts() const;      // 行 × 列文本快照（移动/排序用）
  void applyTableTexts(const QVector<QStringList> &texts); // 回填并刷新行状态
  QVector<WellTopRecord> collectRows(QString *error) const;
  bool isDirty() const;
  bool confirmDiscard();                 // true = 继续切换/关闭
  bool requireCleanTable(const QString &what); // 批量/回滚/合并前的未保存守卫
  void showIssues(const QVector<WellTopsEdit::Issue> &issues);
  void rebuildVersionMenu();
  void setSummaryLine(const QString &text);
  bool commitBatch(QVector<WellTopRecord> rows, const QString &editKind, const QString &confirmText);

  // ESC/标题栏关闭与「关闭」按钮同走丢弃确认（防未保存改动静默丢失）。
  void reject() override;

  DataCatalog *m_catalog = nullptr;
  QString m_assetId;
  QString m_displayName;
  std::unique_ptr<WellTopsEditorWorkflow> m_workflow;

  QVector<WellTopRecord> m_allRows;    // 当前版本全文件行（其他井原样）
  QVector<WellTopRecord> m_baseline;   // 当前井的基线行（脏行比对）
  QString m_currentWell;               // 编辑表当前呈现的井（combo 显示形）
  QString m_pendingMergeNote;          // 合并进表未保存时的溯源（保存时入版本 extra）
  bool m_filling = false;
  bool m_readOnly = false;
  bool m_confirmingReject = false;

  QComboBox *m_wellCombo = nullptr;
  QLabel *m_assetLabel = nullptr;
  QLabel *m_summaryLine = nullptr;
  QPushButton *m_validateButton = nullptr;
  QPushButton *m_saveButton = nullptr;
  QPushButton *m_mergeButton = nullptr;
  QToolButton *m_batchButton = nullptr;
  QToolButton *m_versionButton = nullptr;
  QListWidget *m_issueList = nullptr;
  QLabel *m_issueTitle = nullptr;
  QTableWidget *m_table = nullptr;
};
