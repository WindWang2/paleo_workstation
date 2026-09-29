// 层：视图
#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include <QList>

#include "domain/wellcompositemodel.h"
#include "wellcompositestore.h"

// ui/wellcomposite/stratassignment — D3.6 地层单元显式指派
//
// 给未识别层名（如 A/B/C1 标志层）指派系/统/组/段——必须用户显式指派，程序
// 不猜；指派存 sidecar，不污染源数据。词表来自国际年代色标表
// （chronostratcolors：系 14 + 统 40+ + 区域组 13）。

class QTableWidget;

namespace WellComposite
{

namespace StratAssign
{
// 找出 stratigraphyIntervals 中系/统为空的「未识别层名」清单（按首次出现序）
QStringList unrecognizedLayerNames(const QVector<StratigraphyInterval> &intervals);

// 应用指派到地层组合区间（只填空，不覆盖已识别）
QVector<StratigraphyInterval> applyAssignments(const QVector<StratigraphyInterval> &intervals,
                                                const QList<StratAssignment> &assignments);
} // namespace StratAssign

// D3.6 指派对话框：表列 = 层名 | 系 | 统 | 组 | 段（级联下拉，词表查表）
class StratAssignmentDialog : public QDialog
{
  Q_OBJECT

public:
  // layerNames 待指派层名；existing 既有指派（预填）
  StratAssignmentDialog(const QStringList &layerNames,
                        const QList<StratAssignment> &existing, QWidget *parent = nullptr);

  QList<StratAssignment> assignments() const { return m_assignments; }

public slots:
  void accept() override;

  // 测试钩子：直接设置一行指派（行号 = layerNames 序）
  void setRowAssignment(int row, const QString &system, const QString &series,
                        const QString &formation, const QString &member);

private:
  void rebuildRow(int row);

  QStringList m_layerNames;
  QList<StratAssignment> m_assignments;
  QTableWidget *m_table = nullptr;
};

} // namespace WellComposite
