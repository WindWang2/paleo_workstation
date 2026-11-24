// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// services/agentnavigationaccess.h — I4：语义导航的宿主访问接口（纯 Qt 契约）。
//
// 三个接口只带纯 Qt 类型，供功能层（workflow/agentnavigation 编排）、
// QGIS 封装层（qgis/agentmapnavigation 主画布定位）与视图层
//（ui/agentnavigationadapter 选择 + 原生预览页）三方共同实现/消费——接口
// 落数据层避免 qgis→workflow 的模块依赖环（层契约：QGIS 封装只依赖数据层
// + QtWidgets + qgs 头）。组装根负责把实现注入编排器。
//
// 语义边界（任务书 I4）：选择/定位/预览是临时视图态——效果恒 Read，不写
// Catalog 业务状态、不改 Presentation 写门、不改 renderer/可见性/样式。

namespace paleo::agent::navigation {

// 工具回执 detail 的净化：绝对路径 token 一律折叠为 [path]（任务书 W9
// 「不含源路径」——错误原文可能携带 QFile::errorString 的宿主绝对路径）。
inline QString sanitizeModelText(const QString &text)
{
  QString out;
  const QStringList tokens = text.split(QLatin1Char(' '));
  for (int i = 0; i < tokens.size(); ++i) {
    const QString &token = tokens.at(i);
    if (token.startsWith(QLatin1Char('/')) && token.size() > 1)
      out += QStringLiteral("[path]");
    else
      out += token;
    if (i + 1 < tokens.size())
      out += QLatin1Char(' ');
  }
  return out;
}

// 当前业务选井的读/替换/清除（实现：ui/agentnavigationadapter）。
class SelectionAccess
{
public:
  virtual ~SelectionAccess() = default;
  // 空串 = 可用；非空 = 机器错误码（如 window_unavailable）。
  virtual QString unavailabilityReason() const = 0;
  // {wells:[id...], origin, token, empty}——token 是宿主生成的选择修订条件
  //（内容摘要；模型只能原样回传，不能自造）。
  virtual QJsonObject snapshot() const = 0;
  // 精确替换井选择（调用前已整批核实为井业务 ID）；回读真实选择结果。
  virtual QJsonObject select(const QStringList &wellIds) = 0;
  // 只清井选择域；调用前的 token/异域判定在编排层完成。
  virtual QJsonObject clear() = 0;
};

// 主画布视野的读/定位（实现：qgis/agentmapnavigation）。
class MapAccess
{
public:
  // 井定位目标：id 供回执/可见性报告；坐标是宿主解析的工程网格局部米
  //（与 QgisCanvasController::zoomToPoint 同一源域）。无坐标井在编排层
  // 先行拒绝，不进本接口。
  struct WellTarget
  {
    QString id;
    double x = 0, y = 0;
  };
  virtual ~MapAccess() = default;
  virtual QString unavailabilityReason() const = 0;
  // {canvas_present, extent:[...], crs, has_layers}——不创建画布。
  virtual QJsonObject snapshot() const = 0;
  // 坐标/源 CRS 由宿主给出，严格转换失败即拒绝（旧视野不动，不回落原始
  // 坐标——区别于 zoomToLayer 的 UI 回退行为）。
  virtual QJsonObject locateWells(const QVector<WellTarget> &wells, bool fit) = 0;
  // 受管图层（仅已实例化层；未实例化/已失效如实拒绝，不实例化非当前层位）。
  virtual QJsonObject locateLayer(const QString &layerId, bool fit) = 0;
};

// 原生井资料/地震测线预览页（实现：ui/agentnavigationadapter →
// DataPreviewTabs 的 typed 接缝）。
class PreviewAccess
{
public:
  virtual ~PreviewAccess() = default;
  virtual QString unavailabilityReason() const = 0;
  // 已打开预览目标的有限快照（≤8 项；含版本/井/测线与加载/错误态）。
  // 命名与 SelectionAccess::snapshot 区分（双继承适配器需要两个具名面）。
  virtual QJsonObject previewSnapshot() const = 0;
  // 异步打开：done 恰好在 GUI 线程回调一次（shown/partial/failed/
  // superseded/cancelled/rejected 由实现归类；同步可判的拒绝也在 done 内
  // 回报）。返回值仅用于 provider 本身缺位的场景（此时 done 不会被调用）。
  using Completion = std::function<void(const QJsonObject &)>;
  virtual QJsonObject openWellView(const QString &versionId, const QString &wellId,
                                   const Completion &done) = 0;
  virtual QJsonObject openSeismicLine(const QString &versionId, const QString &axis,
                                      int lineNumber, const Completion &done) = 0;
};

} // namespace paleo::agent::navigation
