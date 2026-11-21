// 层：数据
#pragma once
#include "catalog/datacatalog.h"
#include "crossplotsamples.h"
#include "metadata/layermanifest.h"
#include "welllogset.h"
#include "wellreadiness.h"
namespace paleo::crossplot {
struct SourceSpec {
  SourceChoice choice;
  QString path, curve, versionId, layerId, sha256;
  Location well;
  bool managed = true;
  bool timeHorizon = false;
};
class CrossplotSources {
public:
  // A-W3：通道清单的快照准备 → 后台读取 → 受控应用三段式。
  // snapshotCatalog 在 catalog 所属线程取轻量只读输入（实体/链接/版本/
  // 元数据/图层声明，不做文件 I/O）；buildInventory 只消费快照与源文件，
  // 可在工作线程运行（表头走 WellLogSet 头缓存，就绪判定走
  // computeReadiness 的纯文件/模型侧）。inventory 保持同步兼容口径
  //（= buildInventory ∘ snapshotCatalog），供既有调用方与测试。
  struct WellSnapshot {
    QString id, name;
    double surfaceX = 0, surfaceY = 0;
    bool hasSurface = false;
    WellReadinessInput readiness;
    QVector<WellLogSet::WellLogLinkSnapshot> logs;
  };
  struct AssetSnapshot {
    QString assetId, displayName, type;
    QString versionId, path, sha256;
    bool managed = true;
  };
  struct CatalogSnapshot {
    QString projectDir;
    quint64 catalogMutationSeq = 0;
    QVector<WellSnapshot> wells;
    QVector<AssetSnapshot> assets;
    QVector<LayerDeclaration> declarations;
    bool valid = false; // catalog 存在且已打开
  };
  struct InventoryOutcome {
    QVector<SourceSpec> sources;
    int filesPlanned = 0;  // 计划解析的 well_log 文件数
    int filesFailed = 0;   // 缺失或表头解析失败的文件数
    QStringList failedFiles;
  };
  static CatalogSnapshot snapshotCatalog(DataCatalog *, const QString &projectDir,
                                         const QVector<LayerDeclaration> &decls);
  static InventoryOutcome buildInventory(const CatalogSnapshot &,
                                         const cluster::Control & = {});
  static QVector<SourceSpec> inventory(DataCatalog *, const QString &projectDir,
                                       const QVector<LayerDeclaration> &);
  static SampleResult load(const QVector<SourceSpec> &,
                           const cluster::Control & = {});
  // Reads existing SATR, validates its version/payload, and uses its source
  // survey.
  static bool attributeSection(const QString &path, AttributeSection *,
                               QString *error, const cluster::Control & = {});
};
} // namespace paleo::crossplot
