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

// 井道配置项（可代表画布上的任意井道：标尺道、地层系统组道、地层分层道、岩性道、曲线道、沉积相道等）
struct WellTrackConfigItem
{
  std::shared_ptr<WellTrack> trackRef; // 指向原井道（非曲线道直接保留，已存在曲线道作为参考）
  TrackType type = TrackType::Curve;
  QString title;
  qreal width = 160.0;
  bool visible = true;
  QVector<CurveData> curves; // 当 type == TrackType::Curve 时，道内所包含的 1-4 根测井曲线
};

// 保持向下兼容别名
using TrackConfigItem = WellTrackConfigItem;

// ui/wellcomposite/ — CurveConfigDialog: 测井道配置与排列管理对话框
//
// 满足石油地质绘图规范与用户需求：
// 1. 列表中直观展现所有测井道（标尺道、地层道、岩性道、曲线道、沉积相道等）及其当前顺序与可见性；
// 2. 支持任意井道位置调整（上移、下移、置顶、置底）；
// 3. 支持曲线道多选 2-4 根曲线合并为同一道显示（同道多曲线叠合）；
// 4. 支持将多曲线道解散为独立单曲线道（每道 1 根曲线）；
// 5. 支持单个曲线从多曲线道拆出为独立道；
// 6. 支持井道重命名与显示隐藏；
// 7. 支持一键恢复标准地质组合（岩性/三孔隙/电阻率/辅助）。
class CurveConfigDialog : public QDialog
{
  Q_OBJECT

public:
  explicit CurveConfigDialog(WellCompositeCanvas *canvas, QWidget *parent = nullptr);
  ~CurveConfigDialog() override = default;

  // 将配置应用到画布
  void applyConfiguration();

  // 获取当前配置列表（供测试验证）
  const QList<WellTrackConfigItem> &trackItems() const { return m_tracks; }

  // 业务逻辑接口（供槽函数及单元测试直接调用）
  bool combineCurves(const QList<QPair<int, int>> &indices, const QString &title);
  bool dissolveTrack(int trackIdx);
  bool extractCurve(int trackIdx, int curveIdx);
  bool renameTrack(int trackIdx, const QString &newTitle);
  void moveTrack(int fromIdx, int toIdx);
  void setTrackVisible(int trackIdx, bool visible);

public slots:
  void onCombineSelected();   // 合并选中的多根曲线为新道（2-4根）
  void onDissolveSelected();  // 解散选中的道为独立单道
  void onExtractCurve();      // 将选中曲线拆分为独立道
  void onMoveTrackUp();       // 井道上移
  void onMoveTrackDown();     // 井道下移
  void onMoveTrackToTop();    // 井道置顶
  void onMoveTrackToBottom(); // 井道置底
  void onRenameSelected();    // 井道重命名
  void onResetDefault();      // 恢复标准地质组合

private slots:
  void onTreeSelectionChanged();
  void onTreeItemChanged(QTreeWidgetItem *item, int column);

private:
  void setupUi();
  void loadFromCanvas();
  void populateTree(int selectTrackIdx = -1);
  void updateButtonStates();

  WellCompositeCanvas *m_canvas = nullptr;

  QTreeWidget *m_tree = nullptr;
  QPushButton *m_btnMoveTop = nullptr;
  QPushButton *m_btnMoveUp = nullptr;
  QPushButton *m_btnMoveDown = nullptr;
  QPushButton *m_btnMoveBottom = nullptr;
  QPushButton *m_btnRename = nullptr;
  QPushButton *m_btnCombine = nullptr;
  QPushButton *m_btnDissolve = nullptr;
  QPushButton *m_btnExtract = nullptr;
  QPushButton *m_btnResetDefault = nullptr;
  QPushButton *m_btnApply = nullptr;
  QLabel *m_lblInfo = nullptr;

  QList<WellTrackConfigItem> m_tracks;
  QVector<CurveData> m_allCurves; // 备份供默认重置
};

// 测井道配置管理对话框别名
using WellTrackConfigDialog = CurveConfigDialog;

} // namespace WellComposite
