#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "../metadata/mapversionstore.h"

class QgisLayerService;

// workflow/mapversioncontroller — 「保存版本 / 发布 / Published」的编排
// （wave/mapping-pipeline 阶段E；PALEO_QGIS_PLAN §41.7 + §1223 最小语义）。
//
//   保存版本 = 该层位全部矢量图层的编辑会话 commit（QGIS 原生：commit 清
//   undo 栈；此处再显式 clear 兜底，undo 不跨版本边界）→ MapVersionStore
//   版本号递增 + provenance 记录。
//   发布 = result/ 快照（成果 gpkg + 布局 PDF，只读）→ Published；发布门
//   = 该层位已能导出 PDF（store 的 map_products 有记录，导出成功后由调用
//   方 recordLayoutProduct）。Published 快照只读；继续编辑产生下一版本，
//   不回写已发布快照。
class MapVersionController : public QObject
{
  Q_OBJECT
  public:
    MapVersionController( MapVersionStore *store, QgisLayerService *layers,
                          QObject *parent = nullptr );

    MapVersion saveVersion( const QString &horizon, const QVariantMap &provenance,
                            QString *error = nullptr );
    QString publish( const QString &horizon, QString *error = nullptr );

  signals:
    void versionSaved( const QString &horizon, int version );
    void published( const QString &horizon, int version, const QString &snapshotDir );

  private:
    MapVersionStore *m_store = nullptr;
    QgisLayerService *m_layers = nullptr;
};
