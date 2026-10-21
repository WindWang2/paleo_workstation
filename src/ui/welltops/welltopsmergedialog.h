// 层：视图
#pragma once
#include <QDialog>
#include <QString>
#include <QVector>

#include "../../domain/wellrecords.h"
#include "../../domain/welltopsedit.h"

class QLabel;
class QTableWidget;

// ui/welltops — 方向 32 导入融合对话框：再导入同井分层的逐行取舍。
// 冲突行默认「保留旧值」（不默认覆盖）；仅新行默认「新增」；仅旧行默认
// 「保留」。确认后 resolvedRows() 带取舍返回，落库仍归编辑器保存动作。
class WellTopsMergeDialog : public QDialog
{
  Q_OBJECT
public:
  explicit WellTopsMergeDialog(const QVector<WellTopsEdit::MergeRow> &rows,
                               QWidget *parent = nullptr);

  // 用户取舍后的合并行（applyMerge 的输入；未 accept 也可读最后状态）。
  QVector<WellTopsEdit::MergeRow> resolvedRows() const;

private:
  void rebuildSummary();
  // 冲突行取值列文本（L1.5：走 tr——英文界面不漏「空」）。静态：调用点在
  // 构造（成员函数）内，不需要 this。
  static QString valueText(const WellTopRecord &r);

  QVector<WellTopsEdit::MergeRow> m_rows;
  QTableWidget *m_table = nullptr;
  QLabel *m_summary = nullptr;
};
