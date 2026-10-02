// 层：视图
#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSlider;
class QSpinBox;
class QToolButton;

// 属性建模参数面板（goal/property-modeling）。只发意图：层位名、曲线、
// 层数、聚合器、IDW 幂次、叠加透明度。格架与充填在功能层。
// 聚合器序号与 paleo::stratgrid::Aggregator 一致：
//   0 均值 / 1 弧长加权均值（缺省）/ 2 中位数 / 3 众数。
class PropertyModelPanel : public QWidget
{
  Q_OBJECT
public:
  explicit PropertyModelPanel(QWidget *parent = nullptr);

  QString topHorizon() const;
  QString bottomHorizon() const;
  QString curveMnemonic() const;
  int layerCount() const;
  int aggregator() const;
  double idwPower() const;
  double overlayAlpha() const;

  void setBusy(bool busy);
  bool isBusy() const { return m_busy; }

signals:
  void buildRequested(const QString &topHorizon, const QString &bottomHorizon,
                      const QString &curveMnemonic, int layerCount, int aggregator,
                      double idwPower, double overlayAlpha);
  void cancelRequested();
  void alphaChanged(double alpha);

public slots:
  void updateProgress(int percent, const QString &stageLabel);
  void showResult(bool ok, const QString &summary);

private:
  void buildUi();
  void syncEnabledState();

  QLineEdit *m_top = nullptr;
  QLineEdit *m_bot = nullptr;
  QLineEdit *m_curve = nullptr;
  QSpinBox *m_layers = nullptr;
  QComboBox *m_agg = nullptr;
  QDoubleSpinBox *m_power = nullptr;
  QSlider *m_alpha = nullptr;
  QToolButton *m_build = nullptr;
  QToolButton *m_cancel = nullptr;
  QProgressBar *m_progress = nullptr;
  QLabel *m_status = nullptr;
  bool m_busy = false;
};
