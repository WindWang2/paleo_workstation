// 层：QGIS 封装
#pragma once
#include <QList>
#include <QString>
#include <QVector>

#include <functional>

class QgsCoordinateReferenceSystem;
class QgsMapCanvas;
class QgsProject;
class QgsMapLayer;
class QObject;

// 在线 XYZ 瓦片底图（街道/卫星/地形）：工程不自带离线底图时的开源免费兜底。
// 源清单 = 内置三项 + 用户设置 QSettings「basemap/onlineSources」追加的
// 自定义源（JSON 数组，元素 {id,title,url,attribution,zmax}）。瓦片按 Web
// 墨卡托出图，只在工程有真实地理/投影坐标系时可叠加；工程坐标系（ENGCRS
// 本地网格）上拒绝叠加，避免错位。
namespace paleo::onlinebasemap {
struct Source {
  QString id;          // 稳定键（none/osm/esri_imagery/opentopomap/自定义）
  QString title;       // 显示名
  QString url;         // XYZ 模板：{x}/{y}/{z}
  QString attribution; // 版权/条款，必须随图显示
  int zmax = 19;
};

QString noneId();
// 内置源 + 设置里的自定义源（无效项跳过）；不含「无」。
QVector<Source> sources();
Source sourceById(const QString &id);
// 能否叠加在线底图；否时 reason 写明用户可读原因。
bool canOverlay(const QgsCoordinateReferenceSystem &crs, QString *reason = nullptr);
bool canOverlay(const QgsMapCanvas *canvas, QString *reason = nullptr);
// 在线底图渲染失败（离线/被墙/超时）回调；timeoutMs 内画布仍未出完图也报超时。
// 渲染在 QGIS 后台线程，界面不阻塞；回调只给用户可读提示；连接随 layer 销毁断开。
void watchFailures(QgsMapCanvas *canvas, QgsMapLayer *layer, int timeoutMs,
                   const std::function<void(const QString &)> &onFailure);
QString xyzUri(const Source &source);
// 建 XYZ 栅格层（customProperty paleoOnlineBasemap/paleoBasemapAttribution 已设）。
QgsMapLayer *createLayer(const Source &source, QObject *parent = nullptr);
void destroyLayer(QgsMapLayer *layer); // deleteLater
// 层列表里的在线底图层（无则空）——不缓存裸指针，层可能已被 TOC 移除。
QgsMapLayer *findLayer(const QList<QgsMapLayer *> &layers);
// 每工程记忆：写入工程条目 + 本机设置（按工程路径分键）。
QString savedChoice(const QgsProject *project);
void saveChoice(QgsProject *project, const QString &id);
} // namespace paleo::onlinebasemap
