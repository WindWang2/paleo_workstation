// 层：数据
#pragma once
#include <QString>
#include <QVector>

#include "datacatalog.h"

#include "../domain/sequenceframework.h"

// catalog/ — 层序地层格架的落库面（方向 28）。
//
// 「格架数据走 catalog 实体 + 版本，禁止旁路存储」——格架 JSON 是 catalog
// 的一个受管资产版本（type=sequence_framework，stage=OUTPUT，受管路径
// {stage}/{asset_id}/{version_id}/framework.json），每次保存涨一个版本号；
// 单元与层序界面的归属用 EntityAssetLink（entityType=sequence_boundary、
// role=framework_unit）显式建模——关系从不由名字推断，但名字（mappingHorizons()
// 成员）仍是层位序的唯一权威，链接只是把它钉到实体上。
//
// 语义边界：本类只读/写格架资产，不碰 sequence_boundary 实体本身——既有
// 「层序界面」实体（sb-<NAME>、extra["pending"]）语义完全不变。
namespace SequenceFramework
{

class FrameworkStore
{
  public:
    FrameworkStore( DataCatalog *catalog, const QString &projectDir );

    DataCatalog *catalog() const { return m_catalog; }
    QString projectDir() const { return m_projectDir; }

    // 词表常量（catalog 侧单一真源；角色在 RoleRegistry 里同名登记）。
    static QString assetType() { return QStringLiteral( "sequence_framework" ); }
    // EntityAssetLink.role——词表登记见 catalog/roleregistry.cpp。
    static QString unitRole() { return QStringLiteral( "framework_unit" ); }
    static QString payloadFileName() { return QStringLiteral( "framework.json" ); }
    // sequence_boundary 实体 id 形态（与 io/dataimportservice.cpp 同款，不改）。
    static QString boundaryEntityId( const QString &horizon );

    // 已有格架资产 id；无 → 空串（不是错误：工程还没建格架）。
    QString assetId() const;
    // 该资产的现有版本数（无资产 = 0）。
    int versionCount() const;
    bool hasFramework() const { return !assetId().isEmpty(); }

    // 读取：无格架 → 空 Framework 且 error 留空（与「文件不存在不是错误」
    // 同款约定）；有而解析失败/读盘失败 → false + error。
    bool load( Framework *out, QString *error = nullptr ) const;

    // 保存：写新版本 + 同步 sequence_boundary 归属链接。整批走 BatchSave
    //（一次落盘，不留「版本入库但链接没落盘」的半截状态）。
    bool save( const Framework &fw, QString *error = nullptr );

  private:
    DataCatalog *m_catalog = nullptr;
    QString m_projectDir;
};

} // namespace SequenceFramework
