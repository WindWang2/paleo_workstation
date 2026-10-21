// 层：QGIS 封装
#pragma once
#include <QList>
#include <QString>

class QgsCoordinateReferenceSystem;
class QgsCoordinateTransformContext;
class QgsMapLayer;
class QgsProject;
class QObject;
class QgsMapCanvas;
#include <QPair>
#include <functional>

// 井位类视图共用的「GIS 底图」来源（在线 XYZ 预置源之外的扩展）：
//   - 自定义 XYZ 瓦片 URL（http/https/file 模板，须含 {x}{y}{z}）；
//   - 本地栅格/矢量文件（GDAL/OGR 可读，含 .mbtiles）；
//   - 工程里已有的 GIS 图层（克隆为预览私有实例，不改动工程）。
// 统一编码为「选择串」以便按工程记忆：预置源 id（osm/…）、none、
// xyz:<url>、file:<绝对路径>、layer:<工程图层 id>。
// 所有底图层都打 paleoBasemap/paleoOnlineBasemap 标记：全图范围不计入、
// 换底图时可按标记找回旧层。
namespace paleo::basemap {
enum class Kind { None, Preset, Xyz, File, ProjectLayer };
struct Choice {
  Kind kind = Kind::None;
  QString value; // 预置 id / URL / 路径 / 图层 id
};
Choice decode(const QString &spec);
QString encode(const Choice &choice);

// URL 模板校验（scheme 仅 http/https/file，必须含 {x}{y}{z}）。
bool isValidXyzUrl(const QString &url, QString *why = nullptr);
// 用户可读的来源短名：URL 只露主机名（查询串里可能带密钥，不显示）。
QString displayName(const Choice &choice, const QgsProject *project = nullptr);

QgsMapLayer *createXyz(const QString &url, QObject *parent, QString *error = nullptr);
QgsMapLayer *createFromFile(const QString &path, QObject *parent, QString *error = nullptr);
QgsMapLayer *cloneProjectLayer(const QgsMapLayer *source, QObject *parent, QString *error = nullptr);
// 工程里可作底图的图层（有效栅格/矢量层），按图层树顺序。
QList<QgsMapLayer *> projectCandidates(const QgsProject *project);
// 打底图标记（id 为选择串，attribution 随图显示）。
void mark(QgsMapLayer *layer, const QString &id, const QString &attribution);

// 坐标系检查：底图能否叠加到画布坐标系。
//   - XYZ 瓦片：画布须为真实地理/投影坐标系（工程坐标系/未设置 → 拒绝）；
//   - 底图无坐标系：按画布坐标系直接叠加并警告（可能错位）；
//   - 坐标系不同：按画布转换上下文实时重投影；无法转换（如未配准的工程
//     坐标系 ↔ 地理坐标系）→ 拒绝并说明。
// 返回 false = 拒绝；message 写用户可读原因或警告（可为空）。
bool checkCrs(QgsMapLayer *layer, const QgsCoordinateReferenceSystem &canvasCrs,
              const QgsCoordinateTransformContext &context, QString *message);

// ---- 视图层门面（视图不直连 qgs*：工程/图层以 QObject 传入）----
// 按选择串建底图层并过坐标系检查：成功返回层（父对象 parent），message 为
// 警告（可空）；失败返回空，message 为原因。none 返回空且 message 为空。
QgsMapLayer *build(const QString &spec, QgsMapCanvas *canvas, QObject *project, QObject *parent,
                   QString *message);
QString displayName(const QString &spec, QObject *project);
// 工程候选图层（id, 名称）。
QList<QPair<QString, QString>> projectCandidateNames(QObject *project);
QString savedChoice(QObject *project);
void saveChoice(QObject *project, const QString &spec);
// 画布坐标系变化回调（连接随 context 销毁断开）。
void onCanvasCrsChanged(QgsMapCanvas *canvas, QObject *context, const std::function<void()> &callback);
} // namespace paleo::basemap
