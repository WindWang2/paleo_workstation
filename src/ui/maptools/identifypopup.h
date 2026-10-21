// 层：视图
#pragma once

#include "../../qgis/identifytool.h"

#include <QFrame>

class QLabel;
class QToolButton;
class QVBoxLayout;

// 画布上的悬浮信息。多个命中时用上一处/下一处切换，不另开停靠面板。
class IdentifyPopup : public QFrame, public IdentifyPresenter
{
  Q_OBJECT
public:
  explicit IdentifyPopup(QWidget *parent = nullptr);
  void setHits(const QVector<IdentifyHit> &hits);
  int index() const { return m_index; }
  int count() const { return m_hits.size(); }
  void showNear(const QPoint &anchor);

  // IdentifyPresenter
  void presentHits(const QVector<IdentifyHit> &hits, const QPoint &anchor) override;
  void dismissHits() override;
  void stepHit(int delta) override { showIndex(m_index + delta); }
  bool hitsShown() const override { return isVisible(); }
  bool pointerOverHits() const override { return m_pointerInside; }

signals:
  void pointerInside(bool inside);

protected:
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  void showIndex(int index);

  QVector<IdentifyHit> m_hits;
  int m_index = 0;
  bool m_pointerInside = false;
  QLabel *m_title = nullptr;
  QLabel *m_subtitle = nullptr;
  QLabel *m_counter = nullptr;
  QWidget *m_switchRow = nullptr;
  QVBoxLayout *m_fields = nullptr;
  QToolButton *m_prev = nullptr;
  QToolButton *m_next = nullptr;
};
