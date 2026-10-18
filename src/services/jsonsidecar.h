// 层：数据
// #482 收敛：JSON 落盘/装载的单一 helper——视图树内嵌持久化（井组合 sidecar
// .wc.json、undo 库 .paleo/undo_stack.json、dataops 各 sidecar）此前在三个
// 目录各写一份「QSaveFile 原子写 + 损坏清洗留痕」，契约散落；此处统一为
// 一份（目录不搬，持久化细节归数据层工具）。header-only：paleo_services
// target 只登记 .cpp，纯内联头无需 CMake 改动。
//
// #534 收敛（续）：三类 sidecar 的 load/save 收成数据层显式门面（见文末
// undoVault*/dataopsSidecar*/wellcompositeSidecar*）——目录推导、原子写时序、
// 损坏清洗口径全部归数据层；视图只经门面表达「读/写哪类 sidecar」的意图，
// 不再自持路径与 QSaveFile（UI_LAYER_PLAN §1「视图不写工程文件」）。
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>

#include "../catalog/datacatalog.h"

namespace paleo {

// 读 JSON 文档（损坏清洗：静默回空会把「有回收清单/有标签/有可撤销挂接」
// 读成「从未有过」，必须 qWarning 留痕）。
//   · 文件不存在 → 空 doc，*ok=true（合法初始态，调用方按空处理）；
//   · 打不开/损坏 → 空 doc，*ok=false（已留痕）；
//   · 正常 → doc，*ok=true。ok 可空。
// context 是日志定位前缀（const char 字面量，如 "dataops sidecar"）——
// 警告形如「<context> corrupt, treated as empty: <path> <原因>」。
inline QJsonDocument readJsonFile( const QString &path, const char *context,
                                   bool *ok = nullptr )
{
  if ( ok )
    *ok = true;
  QFile f( path );
  if ( !f.exists() )
    return QJsonDocument(); // 不存在 = 无 sidecar，正常初始态
  if ( !f.open( QIODevice::ReadOnly ) )
  {
    if ( ok )
      *ok = false;
    qWarning() << context << "unreadable, treated as empty:" << path
               << f.errorString();
    return QJsonDocument();
  }
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &err );
  if ( err.error != QJsonParseError::NoError )
  {
    if ( ok )
      *ok = false;
    qWarning() << context << "corrupt, treated as empty:" << path
               << err.errorString();
    return QJsonDocument();
  }
  return doc;
}

// 原子写：mkpath + QSaveFile——断电/崩溃不留半截 JSON（半截回收清单/undo
// 记录 = 重开静默全丢）；提交失败自动清理临时文件，原文件保持完好。
// 返回 false = 写失败（已 qWarning 留痕，调用方自行决定是否阻断主流程）。
inline bool writeJsonFile( const QString &path, const QJsonDocument &doc,
                           const char *context,
                           QJsonDocument::JsonFormat format = QJsonDocument::Compact )
{
  QDir().mkpath( QFileInfo( path ).absolutePath() );
  QSaveFile f( path );
  if ( !f.open( QIODevice::WriteOnly ) )
  {
    qWarning() << context << "write failed:" << path;
    return false;
  }
  // #540 子项 11：短写必须当失败——半截 JSON 与没写一样糟（下次装载被判
  // 损坏整库作废），而同批 dataexport.cpp:310 已按字节数比对口径执行。
  const QByteArray payload = doc.toJson( format );
  if ( f.write( payload ) != payload.size() )
  {
    qWarning() << context << "write failed:" << path << f.errorString();
    return false;
  }
  if ( !f.commit() )
  {
    qWarning() << context << "commit failed:" << path << f.errorString();
    return false;
  }
  return true;
}

// ---- 三类工程 sidecar 的数据层门面（#534） ----------------------------------
// 目录推导 + 装载/落盘在此收口；视图侧只保留「字段 ↔ 视图值类型」的映射
// （UndoRecord/各 store 都是 ui/ 类型，数据层不得反向 include）。

// 工程目录：catalog 在 <projectDir>/artifacts/metadata/catalog.json → 从该
// 文件纯词法上退三级（metadata → artifacts → projectDir），不查盘上存在性。
// catalog 未开/路径空 → 空串（调用方按「无 sidecar」空转）。
inline QString projectDirForCatalog( DataCatalog *cat )
{
  if ( !cat || !cat->isOpen() || cat->catalogPath().isEmpty() )
    return QString();
  return QFileInfo( QFileInfo( QFileInfo( cat->catalogPath() ).dir().absolutePath() )
                        .dir().absolutePath() )
      .dir()
      .absolutePath();
}

// ---- undo 库（.paleo/undo_stack.json）----
// 方向 30 修正：与 dataopsmodel.h 的 projectDirFor 同步退三级——原两级推导
// 落在 artifacts/ 下。
inline QString undoVaultPath( DataCatalog *cat )
{
  const QString pd = projectDirForCatalog( cat );
  if ( pd.isEmpty() )
    return QString();
  return QDir( pd ).filePath( QStringLiteral( ".paleo/undo_stack.json" ) );
}

// 装载 undo 库根对象 {"undo":[…], "notes":[…]}；ok 语义同 readJsonFile
// （文件不存在 = 空对象 + *ok=true 的合法初始态；打不开/损坏 = 空对象 +
// *ok=false，已 qWarning 留痕）。
inline QJsonObject undoVaultLoad( DataCatalog *cat, bool *ok = nullptr )
{
  const QString path = undoVaultPath( cat );
  if ( path.isEmpty() )
  {
    if ( ok )
      *ok = true; // catalog 未开 = 无库可读，不是错误
    return QJsonObject();
  }
  return readJsonFile( path, "DataPage undo vault", ok ).object();
}

// 落盘 undo 库（调用方装配根对象）；写失败只在日志说明——undo 是增强面，
// 不阻塞挂接本身。返回是否落盘成功。
inline bool undoVaultSave( DataCatalog *cat, const QJsonObject &root )
{
  const QString path = undoVaultPath( cat );
  if ( path.isEmpty() )
    return false;
  return writeJsonFile( path, QJsonDocument( root ), "DataPage undo vault" );
}

// ---- dataops sidecar（.paleo/<name>.json：asset_tags/asset_overrides/
// entity_overrides/recycle_bin）----
inline QString dataopsSidecarPath( DataCatalog *cat, const QString &name )
{
  const QString pd = projectDirForCatalog( cat );
  if ( pd.isEmpty() )
    return QString();
  return QDir( pd ).filePath( QStringLiteral( ".paleo/" ) + name );
}

inline QJsonObject dataopsSidecarLoad( DataCatalog *cat, const QString &name )
{
  const QString path = dataopsSidecarPath( cat, name );
  if ( path.isEmpty() )
    return QJsonObject();
  // 警告文案保持 "dataops sidecar ..." 前缀（测试按此断言）。
  return readJsonFile( path, "dataops sidecar" ).object();
}

inline bool dataopsSidecarSave( DataCatalog *cat, const QString &name,
                                const QJsonObject &root )
{
  const QString path = dataopsSidecarPath( cat, name );
  if ( path.isEmpty() )
    return false;
  return writeJsonFile( path, QJsonDocument( root ), "dataops sidecar" );
}

// ---- 井组合 sidecar（<源名>.wc.json，源数据同目录）----
// 路径由源数据名派生（投影目录推导不适用于它），装载/落盘仍归数据层。
inline QJsonDocument wellcompositeSidecarLoad( const QString &path, bool *ok = nullptr )
{
  return readJsonFile( path, "wellcomposite sidecar", ok );
}

inline bool wellcompositeSidecarSave( const QString &path, const QJsonDocument &doc )
{
  return writeJsonFile( path, doc, "wellcomposite sidecar", QJsonDocument::Indented );
}

} // namespace paleo
