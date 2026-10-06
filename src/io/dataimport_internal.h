// 层：数据
#pragma once

// 方向57：dataimportservice 按格式族拆 TU 的共享基座：错误面 + 文件读取 +
// importOneFile 分支族的公共上下文与族函数声明。全部 inline / 头内声明
//（先例 constraintworkflow_internal.h，LNK2005 教训）。ImportSession 定义见
// dataimportservice.h 文件尾。

#include <QFile>
#include <QFileInfo>
#include <QString>

#include "../catalog/datacatalog.h"
#include "../domain/projectclassifier.h"
#include "dataimportservice.h"

namespace paleo::dataimport_detail
{

inline void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

inline QString readFileOrEmpty(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QString();
  return QString::fromUtf8(f.readAll());
}

// importOneFile 分派壳构造、各族分支只读消费（manifestLayerId 由 horizon
// 族回填——派生时间栅格的图层声明 id，分派壳收尾取用）。
struct FamilyContext
{
  DataCatalog *cat = nullptr;
  ImportSession *s = nullptr;
  QString sourcePath;
  QString stem;
  QString assetId;
  QString versionId;
  ProjectClassification cls;
  QFileInfo fi;
  bool external = false;
  CatalogVersion version;
  QString *error = nullptr;
  QString manifestLayerId;
};

// 井身份解析结果（原 DataImportService 私有嵌套 WellBind 迁此，字段逐字）。
struct WellBind
{
  QString entityId;   // 恰好一个匹配
  bool unresolved = false;
  QStringList candidates; // 0/2+ 候选时的集合
};

// 井身份解析（原私有静态成员 DataImportService::resolveWell 迁此，体逐字；
// 定义在 dataimport_wells.cpp）。族内经 using 声明直呼。
WellBind resolveWell(const DataCatalog *cat, const QString &name);

// 族函数（dataimport_wells/geodata.cpp 定义）：返回错误文案，空串 = 成功。
// 失败记账（recordImportFailed/信号）由 importOneFile 分派壳的 fail 完成。
QString importWellHeadFamily(FamilyContext &ctx);
QString importWellLogFamily(FamilyContext &ctx);
QString importWellTopsFamily(FamilyContext &ctx);
QString importDeviationFamily(FamilyContext &ctx);
QString importHorizonFamily(FamilyContext &ctx);
QString importSeismicFamily(FamilyContext &ctx);
QString importAuxReferenceFamily(FamilyContext &ctx);
QString dedupHorizonRederive(DataCatalog *cat, ImportSession &s,
                             const CatalogVersion &existing, const QString &sourcePath,
                             QString *error);

} // namespace paleo::dataimport_detail
