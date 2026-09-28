// 层：视图
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

#include "wellcompositecanvas.h"
#include "domain/wellcompositemodel.h"

// ui/wellcomposite/ — WellCompositePanel: ResFormStar 风格单井综合柱状图总装面板
//
// 包含置顶工具条（比例尺、缩放控制、井名、深度读数）与多井道画布（WellCompositeCanvas），
// 统一对接 LAS 曲线与 XML/Excel 综合柱状图数据源。

namespace WellComposite
{

class WellPositionLegendWidget;

class WellCompositePanel : public QWidget
{
  Q_OBJECT

public:
  explicit WellCompositePanel(QWidget *parent = nullptr);
  ~WellCompositePanel() override = default;

  WellCompositeCanvas *canvas() const { return m_canvas; }
  WellPositionLegendWidget *legendWidget() const { return m_legendWidget; }

  // 加载并装配中国石油标准综合柱状图 XML (SpreadsheetML)
  bool loadComprehensiveXml(const QString &xmlPath);

  // 加载并装配单井 LAS 曲线（支持关联地层分层道与 1-4 根曲线分道合并显示）
  bool loadLasCurves(const QString &wellName, const QVector<CurveData> &curves,
                     const QVector<FormationInterval> &formations = {});

  // 设置并显示井名；reference=true 时徽章标识为参考井（辅助资料内的井，非测区井序列）
  void setWellName(const QString &name, bool reference = false);
  QString wellName() const { return m_wellName; }
  bool isReferenceWell() const { return m_referenceWell; }

  // 获取当前装配的综合数据
  ComprehensiveWellData currentData() const { return m_data; }

  // 打开曲线道组合/解散管理对话框
  void openCurveConfigDialog();

signals:
  void wellLoaded(const QString &wellName);

private:
  void setupUi();
  void setupTracksFromData(const ComprehensiveWellData &data);

  QString m_wellName;
  bool m_referenceWell = false;
  ComprehensiveWellData m_data;

  QLabel *m_lblWellName = nullptr;
  QComboBox *m_scaleCombo = nullptr;
  QToolButton *m_btnZoomOut = nullptr;
  QLabel *m_lblZoom = nullptr;
  QToolButton *m_btnZoomIn = nullptr;
  QToolButton *m_btnResetZoom = nullptr;
  QToolButton *m_btnConfigCurves = nullptr;
  QLabel *m_lblStatus = nullptr;

  WellCompositeCanvas *m_canvas = nullptr;
  WellPositionLegendWidget *m_legendWidget = nullptr;
};

} // namespace WellComposite