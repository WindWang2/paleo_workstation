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
  // 当前面板参数快照（run/train 共用同一组装口径；selection 仅分类用）。
  ClassificationOptions options() const;
  CrossplotCanvas *canvas() const { return m_canvas; }
signals:
  void samplesRequested(const QStringList &sourceIds);
  void axesRequested(const paleo::crossplot::Axes &);
  void lassoRequested(const QVector<QPointF> &);
  void pointRequested(QPointF);
  void classifyRequested(const paleo::crossplot::ClassificationOptions &);
  void writeRequested();
  void cancelRequested();
  void assignLabelRequested(const QString &className);
  void clearTrainingRequested();
  void trainRequested();
  void methodChanged();
  // 训练相关参数（CV 折数 / kNN 近邻）变更：范围与 validateTrainingSet 下限
  // 耦合（cvFolds ≥ 2、knnNeighbors ≥ 1；另受 QDA 每类维度+1、LDA 自由度
  // 约束），controller 收到后重算训练可用态。
  void paramsChanged();

public slots:
  // 训练按钮三态：enabled=false 时 tooltip=reason（空态禁用先例同
  // constraintpage 分支式文案）；enabled=true 时 reason 留空或附 warnings 摘要
  // （非阻断）。监督族运行按钮的禁用原因只按此口径分流：!enabled 取 reason，
  // enabled 但未训练落固定文案——warnings 摘要绝不冒充禁用原因。
  void setTrainingState(bool enabled, const QString &reason);
  void setTrainingSummary(const QString &summary);
  // 训练质量展示（CV 混淆矩阵 + 每类 precision/recall + warnings，只读；
  // 禁「准确率」字样）。空 report = 清空。
  void setTrainingQuality(const QVariantMap &report);
  // 已训练且与当前方法/标注匹配（controller 依 workflow 门禁语义下发）；
  // 监督族的运行按钮要求「训练态可用且已训练」二者兼备。
  void setModelTrained(bool trained);

private:
  void updateMethodParams();
  QListWidget *m_sources;
  QComboBox *m_x, *m_y, *m_z, *m_method;
  QSpinBox *m_k, *m_class;
  QDoubleSpinBox *m_yaw, *m_pitch;
  QCheckBox *m_standardize;
  CrossplotCanvas *m_canvas;
  QLabel *m_stats, *m_message, *m_classes;
  QProgressBar *m_progress;
  QPushButton *m_load, *m_run, *m_write, *m_cancel;
  // 标注区（自由词套索标注）：类名输入 + 标注/清除 + 训练按钮与摘要。
  QComboBox *m_className;
  QPushButton *m_assign, *m_clearTraining, *m_train;
  QLabel *m_trainingSummary, *m_quality;
  // 方法相关参数：SOM 原型网格、kNN 近邻、监督 CV 折数、伴生掩膜阈值。
  // 掩膜阈值为百分数整型 spin（5–95 ↔ 0.05–0.95，步 5 ↔ 0.05，默认 50 ↔
  // 0.5）——域口径 [0,1] 由 options() 除 100 落定；不增 QDoubleSpinBox
  // （既有 yaw/pitch 计数契约）。
  QSpinBox *m_somWidth, *m_somHeight, *m_knnK, *m_cvFolds, *m_maskThreshold;
  QVector<QPointF> m_lasso;
  bool m_busy = false, m_haveSamples = false, m_classified = false;
  // 训练态（controller 经 setTrainingState/setModelTrained 下发；视图只记
  // 状态不判断）：m_trainEnabled = 当前方法+样本+标注 validate 通过；
  // m_modelTrained = 模型新鲜且与方法匹配。监督族运行按钮要求二者兼备
  // （无监督族不参与者两态，恒可用）。
  bool m_trainEnabled = false, m_modelTrained = false;
  QString m_trainReason;
  // busy 对偶 tooltip 备份（控件, 常态 tooltip）：setBusy 忙/闲切换时统一
  // 换「任务进行中」/还原，参照 m_train/m_cancel 写法。
  QList<QPair<QWidget *, QString>> m_tooltips;
};
} // namespace paleo::crossplot
