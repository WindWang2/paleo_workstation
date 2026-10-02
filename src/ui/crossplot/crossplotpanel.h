// 层：视图
#pragma once
#include "domain/faciesclassification.h"
#include <QImage>
#include <QWidget>
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QProgressBar;
class QSpinBox;
class QCheckBox;
class QDoubleSpinBox;
namespace paleo::crossplot {
class CrossplotCanvas : public QWidget {
  Q_OBJECT
public:
  explicit CrossplotCanvas(QWidget *parent = nullptr);
  void setFrame(const PlotFrame &);
  void setSelection(const QVector<int> &);
  QRectF plotRect() const;
signals:
  void lassoRequested(const QVector<QPointF> &vertices);
  void pointRequested(QPointF point);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void changeEvent(QEvent *) override;

private:
  QPointF normalized(QPointF) const;
  void densityImage();
  PlotFrame m_frame;
  QVector<int> m_selected;
  QVector<QPointF> m_gesture;
  QPointF m_start;
  bool m_dragging = false, m_box = false;
  QImage m_density;
};
class CrossplotPanel : public QWidget {
  Q_OBJECT
public:
  explicit CrossplotPanel(QWidget *parent = nullptr);
  void setSources(const QVector<SourceChoice> &);
  void setDimensions(const QStringList &names);
  void setFrame(const PlotFrame &);
  void setSelection(const Selection &, const QStringList &names);
  void setBusy(bool busy);
  void setProgress(int percent);
  void setMessage(const QString &);
  void setClassified(bool available, const QVector<qint64> &counts = {});
  Axes axes() const;
  CrossplotCanvas *canvas() const { return m_canvas; }
signals:
  void samplesRequested(const QStringList &sourceIds);
  void axesRequested(const paleo::crossplot::Axes &);
  void lassoRequested(const QVector<QPointF> &);
  void pointRequested(QPointF);
  void classifyRequested(const paleo::crossplot::ClassificationOptions &);
  void writeRequested();
  void cancelRequested();

private:
  QListWidget *m_sources;
  QComboBox *m_x, *m_y, *m_z, *m_method;
  QSpinBox *m_k, *m_class;
  QDoubleSpinBox *m_yaw, *m_pitch;
  QCheckBox *m_standardize;
  CrossplotCanvas *m_canvas;
  QLabel *m_stats, *m_message, *m_classes;
  QProgressBar *m_progress;
  QPushButton *m_load, *m_run, *m_write, *m_cancel;
  QVector<QPointF> m_lasso;
  bool m_busy = false, m_haveSamples = false, m_classified = false;
};
} // namespace paleo::crossplot
