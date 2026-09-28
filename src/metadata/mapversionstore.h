// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "layermanifest.h"

// metadata/mapversionstore — 阶段E 版本状态机的落表（wave/mapping-pipeline；
// PROJECT_AREA_PLAN 阶段E §177/§245/§260）。
// 表建在与 LayerManifest 同一个工程 meta sqlite（"project.sqlite"）里：
//
//   map_versions(id, horizon, version, provenance, state, published_path,
//                created_utc, pdf_asset_id, pdf_sha256, residual_summary)
//   map_products(id, horizon, kind, path, created_utc, asset_id, sha256)
//                            — 发布门产物登记（OUTPUT 资产 id + SHA-256）
//
// schema 迁移走最简单的前向兼容：CREATE TABLE IF NOT EXISTS + 逐列
// PRAGMA table_info 检查后 ALTER TABLE ADD COLUMN（新列可空），旧库无感升级。
// 旧 schema 行（无 pdf_asset_id）读成未发布，永远不回写。
// §41.7 状态机最小语义：
//   Editing --保存版本（版本号递增 + provenance）--> Editing(下一版)
//   Editing --发布（result/ 快照）--> Published
//   Published 只读；继续编辑产生下一版本，不回写已发布快照。
struct MapVersion {
  int id = 0;               // 行 id
  QString horizon;
  int version = 0;          // 层位内递增：1,2,3…
  QString provenance;       // JSON（步骤、参数、输入版本）
  QString state;            // "Editing" | "Published" —— 读侧归一：仅当
                            // pdf_asset_id 非空才是 Published；旧库行读回 Editing
  QString publishedPath;    // result/ 快照目录，未发布为空
  QString pdfAssetId;       // 发布门：登记在本行上的 catalog OUTPUT 资产 id（可空）
  QString pdfSha256;        // 该 PDF 的 SHA-256（可空）
  QString residualSummary;  // 发布时冻结的逐井「残差或原因」JSON 快照（可空）
  QString createdUtc;
};

class MapVersionStore
{
  public:
    explicit MapVersionStore(const QString &metaSqlitePath);

    bool open(QString *error = nullptr);          // 建表/补列（幂等）

    int currentVersion(const QString &horizon) const;        // 0 = 无版本
    MapVersion latest(const QString &horizon) const;         // version=0 when none
    QVector<MapVersion> versions(const QString &horizon) const;

    // 版本号递增 + provenance 记录；state 起始 "Editing"。最近一次登记的
    // PDF 产物引用（asset_id + sha256）抄进新的未发布行——导出不改已冻结的
    // 版本行，下一次保存才继承产物引用（§260）。
    MapVersion saveVersion(const QString &horizon, const QString &provenanceJson,
                           QString *error = nullptr);

    // 发布门产物登记：记录该层位导出的 PDF —— 文件路径 + catalog OUTPUT
    // 资产 id + 文件 SHA-256（注册在 mapexport.cpp 的 registerMapPdfAsset，
    // 由调用方在导出成功后调用）。assetId/sha256 可空（旧调用形态）。
    bool recordLayoutProduct(const QString &horizon, const QString &pdfPath,
                             const QString &assetId = QString(),
                             const QString &sha256 = QString(),
                             QString *error = nullptr);
    bool hasLayoutProduct(const QString &horizon) const;
    // 最近一次登记的 PDF 产物路径（发布确认对话展示文件名用）；无 → 空。
    QString latestLayoutProduct(const QString &horizon) const;

    // 残差摘要完整性（发布门同一条款，UI tooltip 复用）：summary 须是
    // MapVersionController::residualSummaryJson 产出的 JSON——
    // {"wells_total":N,"covered":N,"missing":[...],"rows":[...]}。
    // 完整 = wells_total>0 && covered==wells_total && missing 为空。
    static bool residualSummaryComplete(const QString &summaryJson,
                                        int *covered = nullptr, int *total = nullptr);

    // D10（pass-2 批准，L1077「≥15/20 numeric」）：发布门数值残差下限——
    // 完备（每井有残差或原因）之外，rows[] 里 kind=="residual" 的数值行还得
    // 够数。真工区 20 井对应绝对 15；小工区按同一比例 3/4 向下取整、至少 1 口
    // （避免绝对值把小工程永久卡在门外）。numericResidualCount：数值行计数。
    static int requiredNumericResiduals(int wellsTotal);
    static int numericResidualCount(const QString &summaryJson);

    // 发布：要求有版本、未处于 Published、版本行上有 pdf_asset_id+pdf_sha256
    // （发布只读这一行）、且 residualSummary 完整（每口井都有残差或原因；
    // 空串/非完整 JSON → 拒绝并写明缺几口井）。把 decls 中该层位的文件图层
    // 与绑定的 PDF 快照到 <工程目录>/result/<horizon>/v<N>/ 并置只读，
    // 最新版本标 Published、记录快照路径与残差摘要。返回快照目录。
    QString publish(const QString &horizon, const QVector<LayerDeclaration> &decls,
                    const QString &residualSummary, QString *error = nullptr);

    bool isPublished(const QString &horizon) const;
    QString publishedPath(const QString &horizon) const;     // 最新已发布快照

  private:
    QString m_dbPath;
};
