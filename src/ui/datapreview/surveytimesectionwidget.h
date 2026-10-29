// 层：视图
#pragma once
#include <QWidget>
#include "../../services/previewdoc.h"

// 测区全景地图的内嵌「地震时间剖面」预览：数据服务 readSeismicLine 的一条
// 测线振幅（列优先 column*samples+sample），红白蓝变密度色标（数据符号，
// 不进 UI token）。状态：空态 / 加载中 / 失败文案 / 成图；成图随窗口尺寸
// 重排（振幅驻内存，重绘零读取）。
class SurveyTimeSectionWidget : public QWidget
{
  Q_OBJECT
public:
  explicit SurveyTimeSectionWidget(QWidget *parent = nullptr);
  ~SurveyTimeSectionWidget() override;
  QSize minimumSizeHint() const override { return { 240, 120 }; }

  void showLoading(const QString &hint);
  void setResult(const SurveyLinePreview &preview);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  void rebuildImage();

  QString m_hint;          // 空态/加载/失败文案（空 = 有数据）
  QString m_error;
  SurveyLinePreview m_preview;
  class QImage *m_image = nullptr; // 逻辑分辨率位图（paintEvent 拉伸绘制）
  double m_clip = 0;       // 振幅对称削剪限（99.5 分位）
};
