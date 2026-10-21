// 层：视图
#pragma once
#include <QPointer>
#include <QWidget>

class PreviewMapPage;
class QComboBox;
class QLabel;
class QToolButton;

// 井位类地图视图共用的「GIS 底图」控件（#14：所有井位可视化页面都能加载底图）。
// 挂在 PreviewMapPage 工具条扩展条上：
//   - 下拉框：无 / 在线预置源（街道/卫星/地形/设置里的自定义源）；
//   - 「更多底图」菜单：自定义 XYZ 瓦片地址… / 本地栅格或矢量文件… /
//     工程已有 GIS 图层（克隆为本页私有实例，不改动工程）。
// 底图压在最底层；坐标系由 paleo::basemap::checkCrs 把关（实时重投影或
// 明确拒绝/警告）；选择按工程记忆，所有井位视图共用同一选择。
// QGIS 细节全在 src/qgis/basemaplayer、onlinebasemap；本类只做控件与接线。
class PreviewBasemapControl : public QWidget
{
  Q_OBJECT
public:
  // prefix 决定对象名：<prefix>OnlineBasemap（下拉）、<prefix>BasemapMore（菜单按钮）、
  // <prefix>BasemapStatus（内置状态标签）。status 非空时状态写入外部标签。
  // project 为工程对象（QgsProject；视图层不直连 qgs*，按 QObject 传）。
  PreviewBasemapControl(PreviewMapPage *page, QObject *project, const QString &prefix,
                        QLabel *status = nullptr);

  // 应用选择串（none / 预置 id / xyz:URL / file:路径 / layer:图层 id）。
  // 成功返回 true 并按工程记忆；失败保留当前底图并在状态里写原因。
  bool applyChoice(const QString &spec, bool remember = true);
  QString currentChoice() const { return m_current; }
  QString statusText() const;
  QObject *basemapLayer() const; // 当前底图层（无则空）
  QComboBox *comboBox() const { return m_combo; }

private:
  void updateAvailability();
  void setStatus(const QString &text);
  void selectInCombo(const QString &spec);
  void promptXyz();
  void promptFile();
  void populateLayerMenu(class QMenu *menu);

  QPointer<PreviewMapPage> m_page;
  QPointer<QObject> m_project;
  QComboBox *m_combo = nullptr;
  QToolButton *m_more = nullptr;
  QPointer<QLabel> m_status;
  QString m_current;
  QString m_unavailableReason;
};

// 便捷挂载：建控件、挂到 page 工具条并恢复工程记忆的选择。
PreviewBasemapControl *attachPreviewBasemap(PreviewMapPage *page, QObject *project,
                                            const QString &prefix, QLabel *status = nullptr);
