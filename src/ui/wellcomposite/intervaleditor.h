// 层：视图
#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include "domain/wellcompositemodel.h"

// ui/wellcomposite/intervaleditor — D3.4/D3.5/D3.13 岩性与相区间编辑
//
// 纯校验/合并/拆分函数 + 两个小区间编辑对话框：
//   LithoIntervalDialog：岩性名/顶/底/备注（D3.4 选中区间弹表）
//   FaciesIntervalDialog：微相/亚相/相名 + 顶底（D3.5，三级联动校验在外部
//   validateFaciesIntervals 统一做——对话框只收集输入）
// 微相⊂亚相⊂相 的层级校验语义：区间按深度排序后，每个微相区间必须完整落在
// 某个亚相区间内、每个亚相区间完整落在某个相区间内（嵌套不越界）。

class QLineEdit;
class QDoubleSpinBox;

namespace WellComposite
{

// D3.4/D3.13 岩性区间工具
namespace IntervalEditor
{
// 相邻同值区间合并（底=下段顶；返回合并后的新列表，出参给合并对数）
QVector<LithologyInterval> mergeAdjacent(const QVector<LithologyInterval> &intervals, int *mergedPairs);
// 选中区间按界线拆分（depth 在区间内才有效；无效返回原列表）
QVector<LithologyInterval> splitAt(const QVector<LithologyInterval> &intervals, int index,
                                   double atDepth, bool *ok);
// 区间基本合法性：底>顶、不越界重叠（同名相邻重叠也报）
QStringList validateLithoIntervals(const QVector<LithologyInterval> &intervals);

// D3.5 三级联动校验（微相⊂亚相⊂相）
struct FaciesIssue
{
  int index = -1;
  QString message;
};
QVector<FaciesIssue> validateFaciesIntervals(const QVector<FaciesInterval> &intervals);

// 区间排序（顶深升序；编辑会话写入前统一）
QVector<LithologyInterval> sortedByDepth(const QVector<LithologyInterval> &intervals);
QVector<FaciesInterval> sortedFaciesByDepth(const QVector<FaciesInterval> &intervals);
} // namespace IntervalEditor

// D3.4 岩性区间编辑对话框
class LithoIntervalDialog : public QDialog
{
  Q_OBJECT
public:
  explicit LithoIntervalDialog(const LithologyInterval &initial, double minDepth,
                               double maxDepth, QWidget *parent = nullptr);
  LithologyInterval interval() const { return m_result; }

public slots:
  void accept() override;

private:
  LithologyInterval m_initial;
  LithologyInterval m_result;
  double m_minDepth;
  double m_maxDepth;
  QLineEdit *m_nameEdit = nullptr;
  QDoubleSpinBox *m_topSpin = nullptr;
  QDoubleSpinBox *m_bottomSpin = nullptr;
  QLineEdit *m_noteEdit = nullptr;
};

// D3.5 相区间编辑对话框（三级名 + 顶底）
class FaciesIntervalDialog : public QDialog
{
  Q_OBJECT
public:
  explicit FaciesIntervalDialog(const FaciesInterval &initial, double minDepth,
                                double maxDepth, QWidget *parent = nullptr);
  FaciesInterval interval() const { return m_result; }

public slots:
  void accept() override;

private:
  FaciesInterval m_initial;
  FaciesInterval m_result;
  double m_minDepth;
  double m_maxDepth;
  QLineEdit *m_majorEdit = nullptr;
  QLineEdit *m_subEdit = nullptr;
  QLineEdit *m_microEdit = nullptr;
  QDoubleSpinBox *m_topSpin = nullptr;
  QDoubleSpinBox *m_bottomSpin = nullptr;
};

} // namespace WellComposite
