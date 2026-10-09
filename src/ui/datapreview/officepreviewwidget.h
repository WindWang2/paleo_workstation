// 层：视图
#pragma once
#include <QWidget>
class OfficePreviewSession;
class QLabel;
class QPushButton;
class QToolButton;
class QComboBox;
class QGraphicsView;
class QGraphicsPixmapItem;
class QResizeEvent;
class QEvent;

class OfficePreviewWidget : public QWidget
{
  Q_OBJECT
public:
  explicit OfficePreviewWidget(const QString &path, const QString &expectedSha = {}, QWidget *parent = nullptr);
  ~OfficePreviewWidget() override;
signals:
  void previewRequested(const QString &path, const QString &expectedSha);
  void pageRequested(int index);
  void previewClosed();
protected:
  void resizeEvent(QResizeEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;
private:
  void open();
  void selectPage(int index);
  void updateZoom();
  QString m_path, m_sha;
  OfficePreviewSession *m_session;
  QLabel *m_status;
  QPushButton *m_retry;
  QToolButton *m_previous, *m_next;
  QComboBox *m_pages, *m_zoom;
  QGraphicsView *m_view;
  QGraphicsPixmapItem *m_image;
  double m_pageScale = 1.0;
};
