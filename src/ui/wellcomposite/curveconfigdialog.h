#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QVector>
#include <memory>

#include "wellcompositetrack.h"
#include "wellcompositecanvas.h"

namespace WellComposite
{

// 曲线道配置项（包含道标题与道内 1-4 根曲线）
struct TrackConfigItem
{
  QString title;
  qreal width = 170.0;
  QVector<CurveData> curves;
};

// ui/wellcomposite/ — CurveConfigDialog: 曲线道组合与解散配置对话框
//
// 满足石油地质绘图规范与用户需求：
// 1. 列表中直观展现所有测井曲线及其所在的测井道；
// 2. 支持多选 2-4 根曲线合并为同一道显示（同道多曲线叠合）；
// 3. 支持将多曲线道解散为独立单曲线道（每道 1 根曲线）；
// 4. 支持单个曲线从多曲线道拆出为独立道；
// 5. 支持井道顺序调整（上移/下移）；
// 6. 支持一键恢复标准地质组合（岩性/三孔隙/电阻率/辅助）。
class CurveConfigDialog : public QDialog
{
  Q_OBJECT

public:
  explicit CurveConfigDialog(WellCompositeCanvas *canvas, QWidget *parent = nullptr);
  ~CurveConfigDialog() override = default;

  // 将配置应用到画布
  void applyConfiguration();

private slots:
  void onCombineSelected();   // 合并选中的多根曲线为新道（2-4根）
  void onDissolveSelected();  // 解散选中的道为独立单道
  void onExtractCurve();      // 将选中曲线拆分为独立道
  void onMoveTrackUp();       // 道上移
  void onMoveTrackDown();     // 道下移
  void onResetDefault();      // 恢复默认组合
  void onTreeSelectionChanged();

private:
  void setupUi();
  void loadFromCanvas();
  void populateTree();
  void updateButtonStates();

  WellCompositeCanvas *m_canvas = nullptr;

  QTreeWidget *m_tree = nullptr;
  QPushButton *m_btnCombine = nullptr;
  QPushButton *m_btnDissolve = nullptr;
  QPushButton *m_btnExtract = nullptr;
  QPushButton *m_btnMoveUp = nullptr;
  QPushButton *m_btnMoveDown = nullptr;
  QPushButton *m_btnResetDefault = nullptr;
  QPushButton *m_btnApply = nullptr;
  QLabel *m_lblInfo = nullptr;

  QList<TrackConfigItem> m_tracks;
  QVector<CurveData> m_allCurves; // 备份供默认重置
};

} // namespace WellComposite
