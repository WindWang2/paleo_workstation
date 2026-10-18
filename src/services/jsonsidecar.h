// 层：数据
// #482 收敛：JSON 落盘/装载的单一 helper——视图树内嵌持久化（井组合 sidecar
// .wc.json、undo 库 .paleo/undo_stack.json、dataops 各 sidecar）此前在三个
// 目录各写一份「QSaveFile 原子写 + 损坏清洗留痕」，契约散落；此处统一为
// 一份（目录不搬，持久化细节归数据层工具）。header-only：paleo_services
// target 只登记 .cpp，纯内联头无需 CMake 改动。
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>

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
  f.write( doc.toJson( format ) );
  if ( !f.commit() )
  {
    qWarning() << context << "commit failed:" << path << f.errorString();
    return false;
  }
  return true;
}

} // namespace paleo
