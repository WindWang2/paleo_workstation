// 层：视图
#pragma once

#include <QWidget>

#include <QList>
#include <QPair>
#include <QStringList>
#include <QVector>

#include "wellcompositepanel.h"

class QListWidget;

// ui/wellcomposite/multiwellview — D5.x 多井对比容器
//
//   D5.1 双井并排（锁步深度滚动可开关 = D2.9 视口同步锁）
//   D5.2 四井 2×2 网格
//   D5.3 同名标志层连线（correlation line 绘制与开关）
//   D5.4 基准面校平：任选标志层作 datum，各井滚动对齐使该标志层同高显示
//   D5.5 井选择器（测区 A1–A20 + 参考井勾选）
//   D5.6 对比模板（井集+道集配置存/取，走 WellCompositeStore sidecar）
//   D5.7 井间层段厚度差表（delta table 侧栏文本）
//
// 容器持有 0–4 个 WellCompositePanel 槽位 + 透明 overlay 绘 correlation 线。

namespace WellComposite
{

class WellSelectionDialog;

class MultiWellView : public QWidget
{
  Q_OBJECT

public:
  enum class LayoutMode
  {
    Single,
    Dual, // 1×2
    Quad  // 2×2
  };

  explicit MultiWellView(QWidget *parent = nullptr);

  void setLayoutMode(LayoutMode mode);
  LayoutMode layoutMode() const { return m_mode; }

  // 占据槽位（槽位数 = 模式容量；返回槽位号，超容量替换末位）
  int setPanel(int slot, WellCompositePanel *panel);
  WellCompositePanel *panelAt(int slot) const;
  int panelCount() const;

  // D5.1/D2.9 锁步滚动开关
  void setLinkScroll(bool link);
  bool linkScroll() const { return m_linkScroll; }

  // D5.3 correlation lines 开关
  void setShowCorrelationLines(bool on);
  bool showCorrelationLines() const { return m_showCorrelation; }
  // 同名标志层连线对（测试观察）：((井A, 名), (井B, 名)) 屏幕连线由 overlay 画
  QVector<QPair<QString, QString>> correlationPairs() const;

  // D5.4 datum 校平：各井滚动使其同名标志层对齐到视口 40% 高度
  bool alignToDatum(const QString &markerName);
  QString datumMarker() const { return m_datumMarker; }
  void clearDatum();

  // D5.5 井选择（容器只接结果；选择对话框由壳/测试打开）
  void setAvailableWells(const QStringList &wells);
  QStringList availableWells() const { return m_availableWells; }
  void setSelectedWells(const QStringList &wells);
  QStringList selectedWells() const { return m_selectedWells; }

  // D5.7 厚度差表文本（井 × 相邻标志层段厚度；TSV）
  QString deltaTableText() const;

signals:
  void layoutModeChanged(LayoutMode mode);
  void linkScrollChanged(bool link);

private slots:
  void onPanelViewportChanged(double top, double bottom, double span);

private:
  int capacity() const;
  void relayout();
  WellCompositePanel *panelForSender(QObject *sender) const;

  LayoutMode m_mode = LayoutMode::Single;
  bool m_linkScroll = true;
  bool m_showCorrelation = true;
  QString m_datumMarker;
  QStringList m_availableWells;
  QStringList m_selectedWells;
  QList<WellCompositePanel *> m_panels; // 槽位数组（可含 nullptr）
  QWidget *m_overlay = nullptr;
  bool m_syncingScroll = false;

  friend class MultiWellOverlayPainter; // overlay 绘制访问
};

// D5.5 井选择对话框：复选列表（测区井 + 参考井分区标注琥珀/蓝语义 D7.10）
class WellSelectionDialog : public QDialog
{
  Q_OBJECT

public:
  // wells: 全部可选井；referenceWells 标注为参考井（分区显示）
  WellSelectionDialog(const QStringList &wells, const QStringList &referenceWells,
                      const QStringList &initialSelection, QWidget *parent = nullptr);

  QStringList selectedWells() const { return m_selected; }

  // 测试钩子
  void toggleWell(const QString &name, bool checked);

private:
  QStringList m_wells;
  QStringList m_referenceWells;
  QStringList m_selected;
  class QListWidget *m_list = nullptr;
};

} // namespace WellComposite
