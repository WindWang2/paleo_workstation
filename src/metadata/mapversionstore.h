#pragma once
#include <QString>
#include <QVector>

#include "layermanifest.h"

// metadata/mapversionstore — 阶段E 版本状态机的落表（wave/mapping-pipeline）。
// 表建在与 LayerManifest 同一个工程 meta sqlite（"project.sqlite"）里：
//
//   map_versions(id, horizon, version, provenance, state, published_path,
//                created_utc)
//   map_products(id, horizon, kind, path, created_utc)   — 发布门产物登记
//
// schema 迁移走最简单的前向兼容：CREATE TABLE IF NOT EXISTS，新字段
// （published_path）可空，旧库无感升级。§41.7 状态机最小语义：
//   Editing --保存版本（版本号递增 + provenance）--> Editing(下一版)
//   Editing --发布（result/ 快照）--> Published
//   Published 只读；继续编辑产生下一版本，不回写已发布快照。
struct MapVersion {
  int id = 0;               // 行 id
  QString horizon;
  int version = 0;          // 层位内递增：1,2,3…
  QString provenance;       // JSON（步骤、参数、输入版本）
  QString state;            // "Editing" | "Published"
  QString publishedPath;    // result/ 快照目录，未发布为空
  QString createdUtc;
};

class MapVersionStore
{
  public:
    explicit MapVersionStore(const QString &metaSqlitePath);

    bool open(QString *error = nullptr);          // 建表（幂等）

    int currentVersion(const QString &horizon) const;        // 0 = 无版本
    MapVersion latest(const QString &horizon) const;         // version=0 when none
    QVector<MapVersion> versions(const QString &horizon) const;

    // 版本号递增 + provenance 记录；state 起始 "Editing"。
    MapVersion saveVersion(const QString &horizon, const QString &provenanceJson,
                           QString *error = nullptr);

    // 发布门：登记/查询该层位的布局产物（阶段E：D61 的 PDF 能导出之后
    // 发布入口才暴露）。
    bool recordLayoutProduct(const QString &horizon, const QString &pdfPath, QString *error = nullptr);
    bool hasLayoutProduct(const QString &horizon) const;

    // 发布：要求有版本、未处于 Published、且 hasLayoutProduct。把 decls 中
    // 该层位的文件图层与登记的 PDF 快照到 <工程目录>/result/<horizon>/v<N>/
    // 并置只读，最新版本标 Published、记录快照路径。返回快照目录。
    QString publish(const QString &horizon, const QVector<LayerDeclaration> &decls,
                    QString *error = nullptr);

    bool isPublished(const QString &horizon) const;
    QString publishedPath(const QString &horizon) const;     // 最新已发布快照

  private:
    QString m_dbPath;
};
