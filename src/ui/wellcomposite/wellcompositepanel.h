#pragma once

#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

#include "wellcompositecanvas.h"
#include "io/wellcompositexml.h"

// ui/wellcomposite/ — WellCompositePanel: ResFormStar 风格单井综合柱状图总装面板
//
// 包含置顶工具条（比例尺、缩放控制、井名、深度读数）与多井道画布（WellCompositeCanvas），
// 统一对接 LAS 曲线与 XML/Excel 综合柱状图数据源。

namespace WellComposite
{

class WellCompositePanel : public QWidget
{
  Q_OBJECT

public:
  explicit WellCompositePanel(QWidget *parent = nullptr);
  ~WellCompositePanel() override = default;

  WellCompositeCanvas *canvas() const { return m_canvas; }

  // 加载并装配中国石油标准综合柱状图 XML (SpreadsheetML)
  bool loadComprehensiveXml(const QString &xmlPath);

  // 加载并装配单井 LAS 曲线
  bool loadLasCurves(const QString &wellName, const QVector<CurveData> &curves);

  // 设置并显示井名
  void setWellName(const QString &name);
  QString wellName() const { return m_wellName; }

  // 获取当前装配的综合数据
  ComprehensiveWellData currentData() const { return m_data; }

signals:
  void wellLoaded(const QString &wellName);

private:
  void setupUi();
  void setupTracksFromData(const ComprehensiveWellData &data);

  QString m_wellName;
  ComprehensiveWellData m_data;

  QLabel *m_lblWellName = nullptr;
  QComboBox *m_scaleCombo = nullptr;
  QToolButton *m_btnZoomOut = nullptr;
  QLabel *m_lblZoom = nullptr;
  QToolButton *m_btnZoomIn = nullptr;
  QToolButton *m_btnResetZoom = nullptr;
  QLabel *m_lblStatus = nullptr;

  WellCompositeCanvas *m_canvas = nullptr;
};

} // namespace WellComposite
