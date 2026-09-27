// 层：功能
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class DataCatalog;

// workflow/ — 派生产物落位 + DERIVED 版本登记（T26；PROJECT_AREA_PLAN L1057）。
// 厚度栅格 / wells.thickness GeoJSON / ONNX 预测 / 约束 IDW / 融合 / 相多边形
// 等派生输出不再写 QDir::temp()（重启即死链，autoplan pass-2 H4 审计），而是
// catalog 受管的 <projectDir>/artifacts/derived/{asset_id}/{version_id}/{filename}
// 并登记 DERIVED 版本：
//   · 资产按 (type, displayName) find-or-create——同一层位厚度栅格重复计算
//     产生同一资产的递增版本，而不是每次一个新资产；
//   · 版本不可变：每次计算发新 ver id（versionNumber = 该资产当前最大 +1），
//     commit 成功后文件置只读（与 import 的 DERIVED 栅格同一纪律）；
//   · provenance：parentVersionIds = 输入版本（如厚度栅格父版本 = D61/D62
//     时间栅格版本），sha256 落库，extra 记生成参数。
// 注册器不持有 catalog（app 里共享 DataImportService 的同一实例——catalog.json
// 是整文件重写，两个实例交错写会互相覆盖）。
struct DerivedStaging
{
  QString absolutePath;  // 落盘目标（父目录已建）
  QString relativePath;  // catalog 版本 path（"artifacts/derived/..."，工程相对）
  QString assetId;
  QString versionId;
  int versionNumber = 0;
  bool isValid() const { return !absolutePath.isEmpty(); }
};

class DerivedAssetRegistrar
{
  public:
    DerivedAssetRegistrar() = default;
    DerivedAssetRegistrar( DataCatalog *catalog, const QString &projectDir );

    bool isBound() const { return m_catalog != nullptr; }
    QString projectDir() const { return m_projectDir; }

    // 计算受管落位：find-or-create 资产、预分配版本 id、建目录。fileName 与
    // displayName 参与受管路径段，非法段（isSafePathSegment）→ 空 staging + error。
    DerivedStaging stage( const QString &assetType, const QString &displayName,
                          const QString &fileName, QString *error = nullptr );

    // 文件已写到 staging.absolutePath 后登记 DERIVED 版本（sha256 现算入库，
    // 成功后置只读）。文件不存在 → false + error，不登记版本行。
    bool commit( const DerivedStaging &st, const QStringList &parentVersionIds,
                 const QString &sourceUri, const QVariantMap &extra,
                 QString *error = nullptr );

    // 算法没有尊重预置 OUTPUT、把文件写到了别处时：先拷进受管路径再 commit。
    // actualPath == staging 路径时等价于 commit。
    bool commitExternal( const DerivedStaging &st, const QString &actualPath,
                         const QStringList &parentVersionIds, const QString &sourceUri,
                         const QVariantMap &extra, QString *error = nullptr );

    // 绝对/工程相对路径 → catalog 版本 id（父版本 provenance 用）。
    // 无匹配的路径不出现在结果里（缺父版本不伪造）。
    QStringList parentVersionIdsFor( const QStringList &paths ) const;

  private:
    DataCatalog *m_catalog = nullptr;
    QString m_projectDir;
    QHash<QString, int> m_pending; // assetId → 已 stage 未 commit 的最大版本号
};
