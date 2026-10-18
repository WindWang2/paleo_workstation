// 层：视图
#pragma once

#include <QFrame>
#include <QPair>
#include <QString>
#include <QVector>

class QLabel;
class QToolButton;
class QVBoxLayout;

// 识别命中的一行展示。工具层把图层要素收成这个结构，弹层不依赖 QGIS 类型。
struct IdentifyHit
{
  QString title;
  QString subtitle;
  QVector<QPair<QString, QString>> fields;
};

// 画布上的悬浮信息。多个命中时用上一处/下一处切换，不另开停靠面板。
class IdentifyPopup : public QFrame
{
  Q_OBJECT
public:
  explicit IdentifyPopup(QWidget *parent = nullptr);
  void setHits(const QVector<IdentifyHit> &hits);
  int index() const { return m_index; }
  int count() const { return m_hits.size(); }
  void showNear(const QPoint &anchor);

signals:
  void pointerInside(bool inside);

protected:
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  void showIndex(int index);

  QVector<IdentifyHit> m_hits;
  int m_index = 0;
  QLabel *m_title = nullptr;
  QLabel *m_subtitle = nullptr;
  QLabel *m_counter = nullptr;
  QWidget *m_switchRow = nullptr;
  QVBoxLayout *m_fields = nullptr;
  QToolButton *m_prev = nullptr;
  QToolButton *m_next = nullptr;
};
