// 层：视图
#include "multiwellview.h"
#include <QScopedValueRollback>

namespace WellComposite {
void MultiWellView::clearPanels() {
  QScopedValueRollback<bool> guard(m_syncingScroll, true);
  for (auto *panel : m_panels) delete panel;
  m_panels.clear();
  m_panels.resize(m_mode == LayoutMode::Row ? 1 : 4);
  relayout();
}
double MultiWellView::displayOffset(const WellCompositePanel *panel) const {
  if (panel && !m_datumMarker.isEmpty())
    for (const auto &marker : panel->canvas()->markerLines())
      if (marker.second == m_datumMarker) return marker.first;
  return 0.0;
}
void MultiWellView::setDisplayDepthRange(double top, double bottom) {
  QScopedValueRollback<bool> guard(m_syncingScroll, true);
  for (auto *panel : m_panels) if (panel) {
    const double offset = displayOffset(panel);
    panel->canvas()->setVisibleDepthRange(top + offset, bottom + offset);
  }
  m_overlay->update();
}
QPair<double, double> MultiWellView::displayDepthRange() const {
  for (auto *panel : m_panels) if (panel) {
    const double offset = displayOffset(panel);
    return {panel->canvas()->visibleTopDepth() - offset, panel->canvas()->visibleBottomDepth() - offset};
  }
  return {0,100};
}
}
