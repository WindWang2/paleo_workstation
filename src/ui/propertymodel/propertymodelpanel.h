// 层：视图
#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSlider;
class QSpinBox;
class QToolButton;

// 属性建模参数面板（goal/property-modeling；V2 = goal/prop-model-v2）。
// 只发意图：层位名、曲线、层数、聚合器、IDW 幂次、叠加透明度；V2 参数
// （方法/变差/种子/实现数/对象建模/相带约束）经只读 getter 供编排层拉取
// 并翻译成 workflow 请求（视图层不 include algorithms——分层禁令）。
// 聚合器序号与 paleo::stratgrid::Aggregator 一致：
//   0 均值 / 1 弧长加权均值（缺省）/ 2 中位数 / 3 众数。
// 方法序号：0 IDW 插值 / 1 序贯高斯模拟。
// 变差类型序号：0 球状 / 1 指数 / 2 高斯。
// 对象类型序号：0 河道 / 1 点坝。
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

  // ---- V2 参数（编排层拉取面）----
  int method() const;               // 0 IDW / 1 SGS
  int variogramType() const;        // 0 球状 / 1 指数 / 2 高斯
  double nugget() const;
  double sill() const;
  double rangeMeters() const;
  double azimuthDeg() const;
  double anisotropyRatio() const;
  double verticalRangeRatio() const;
  int realizations() const;         // [1, 64]
  unsigned long long seed() const;  // mt19937_64
  bool faciesEnabled() const;       // 相带约束（最新相图）
  bool objectEnabled() const;       // 对象建模叠加
  int objectType() const;           // 0 河道 / 1 点坝
  double objectAzimuthDeg() const;
  double objectLengthMeters() const;   // 0 = 自动（0.8×地图对角线）
  double objectWidthMeters() const;
  double objectThicknessMeters() const;
  double objectCurvatureMeters() const;
  double objectValue() const;
  int objectCount() const;
  unsigned long long objectSeed() const;

  void setBusy(bool busy);
  bool isBusy() const { return m_busy; }

  // 口径标签：编排层把诚实口径（竖直近似/竖帘/相带/种子）回写到这里，
  // 面板只呈现不解释。
  void setCaliberNote(const QString &note);
  QString caliberNote() const;

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
  QComboBox *m_method = nullptr;
  QGroupBox *m_sgsGroup = nullptr;
  QComboBox *m_variogram = nullptr;
  QDoubleSpinBox *m_nugget = nullptr;
  QDoubleSpinBox *m_sill = nullptr;
  QDoubleSpinBox *m_range = nullptr;
  QDoubleSpinBox *m_azimuth = nullptr;
  QDoubleSpinBox *m_anisotropy = nullptr;
  QDoubleSpinBox *m_verticalRatio = nullptr;
  QSpinBox *m_realizations = nullptr;
  QLineEdit *m_seed = nullptr;
  QCheckBox *m_facies = nullptr;
  QGroupBox *m_objectGroup = nullptr;
  QComboBox *m_objectType = nullptr;
  QDoubleSpinBox *m_objectAzimuth = nullptr;
  QDoubleSpinBox *m_objectLength = nullptr;
  QDoubleSpinBox *m_objectWidth = nullptr;
  QDoubleSpinBox *m_objectThickness = nullptr;
  QDoubleSpinBox *m_objectCurvature = nullptr;
  QDoubleSpinBox *m_objectValue = nullptr;
  QSpinBox *m_objectCount = nullptr;
  QLineEdit *m_objectSeed = nullptr;
  QSlider *m_alpha = nullptr;
  QToolButton *m_build = nullptr;
  QToolButton *m_cancel = nullptr;
  QProgressBar *m_progress = nullptr;
  QLabel *m_status = nullptr;
  QLabel *m_caliber = nullptr;
  bool m_busy = false;
};
