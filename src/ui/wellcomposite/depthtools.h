// 层：视图
#pragma once

#include <QDialog>
#include <QPair>
#include <QString>
#include <QVector>

#include <QList>

#include "wellcompositestore.h"

// ui/wellcomposite/depthtools — 深度交互工具集（D2.1/D2.7/D2.11/D2.12/D6.3）
//
// 纯函数（吸附/步长/读数/单位换算）+ Ctrl+G 跳深度对话框。
// 画布与面板共用；无状态、可单测。

class QDoubleSpinBox;
class QLabel;

namespace WellComposite
{

namespace DepthTools
{

// 深度刻度线（标志层/marker）便捷别名
using MarkerLine = QPair<double, QString>;

// nice 步长序列（与标尺道一致）：0.5 → 1000
double niceStepFor(double pxPerMeter, double minPixelSpacing = 36.0);

// D2.1 深度吸附：优先标志层线（距离 ≤ 阈值米），其次整刻度网格。
// snapToMarkers/snapToGrid 可分别关断；都不开或无命中返回原值。
double snapDepth(double rawDepth, const QVector<MarkerLine> &markers,
                 double thresholdMeters, bool snapToMarkers, bool snapToGrid,
                 double gridStep);

// D2.11 最近标志层名（距离在 maxDistanceMeters 内才返回非空；出参给距离）
QString nearestMarkerName(double depth, const QVector<MarkerLine> &markers,
                          double maxDistanceMeters, double *distanceOut = nullptr);

// D2.11 读数条文本：大深度 + 最近标志层
QString readoutText(double depth, const QVector<MarkerLine> &markers);

// D2.12 gap 段：相邻标志层间距 > thresholdMeters 的 (top,bottom,name1,name2) 列表
struct GapSegment
{
  double top = 0.0;
  double bottom = 0.0;
  QString upperMarker;
  QString lowerMarker;
  double span = 0.0;
};
QVector<GapSegment> gapSegments(const QVector<MarkerLine> &markers, double thresholdMeters);

// D6.3 单位换算（显示层换算，数据不动）
constexpr double kMetersPerFoot = 0.3048;
inline double metersToFeet(double m) { return m / kMetersPerFoot; }
inline double feetToMeters(double ft) { return ft * kMetersPerFoot; }
QString formatDepth(double depthMeters, bool feetDisplay);

// D2.6 书签管理（列表操作；持久化走 WellCompositeStore）
int addBookmark(QList<DepthBookmark> *bookmarks, const QString &name, double depth);
bool removeBookmark(QList<DepthBookmark> *bookmarks, const QString &name);

// D2.3 钉注管理
int addPin(QList<DepthPin> *pins, double depth, const QString &text);
bool removePinAt(QList<DepthPin> *pins, int index);
bool updatePinText(QList<DepthPin> *pins, int index, const QString &text);

} // namespace DepthTools

// D2.7 Ctrl+G 跳深度对话框：输入深度（当前单位制），带范围校验提示
class GotoDepthDialog : public QDialog
{
  Q_OBJECT

public:
  explicit GotoDepthDialog(double minDepth, double maxDepth, double currentDepth,
                           bool feetDisplay = false, QWidget *parent = nullptr);

  double selectedDepth() const { return m_depth; }

public slots:
  void accept() override;

private:
  double m_minDepth;
  double m_maxDepth;
  bool m_feetDisplay;
  double m_depth;
  QDoubleSpinBox *m_spin = nullptr;
  QLabel *m_lblHint = nullptr;

  friend class GotoDepthDialogTestAccess;
};

} // namespace WellComposite
