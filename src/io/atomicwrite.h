#pragma once
// 层：数据
// 原子整写 helper（QSaveFile 版）：旁写临时文件 → 校验 write()==size →
// commit() 整体替换。任何一步失败目标路径保持原样（旧文件完好或不存在），
// 不留截断/半截文件，也没有「先删旧件再改名」的丢档窗口。
// 与 metadata/atomicfile.h 的 paleoWriteFileAtomic 的区别：本头只依赖 QtCore，
// 不带 <windows.h>（workflow/qgis 等含 QGIS 头的 TU 引入 windows.h 宏有
// Windows 侧编译风险），供散落的「QFile+Truncate / 未校验 write() / 只查 <0」
// 写点统一收口（等价于 mappingworkbench.cpp 内部 writeJson 的口径）。
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QString>

namespace paleo {

/// 原子写字节。mkdirs=true 时先补建父目录。失败返回 false 并写 *error（可空）。
inline bool writeFileAtomic( const QString &path, const QByteArray &bytes,
                             QString *error = nullptr, bool mkdirs = false )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };
  if ( path.isEmpty() )
    return fail( QStringLiteral( "目标路径为空" ) );
  if ( mkdirs && !QDir().mkpath( QFileInfo( path ).absolutePath() ) )
    return fail( QStringLiteral( "无法创建目录：%1" ).arg( QFileInfo( path ).absolutePath() ) );
  QSaveFile f( path );
  // 禁止回落为「直接覆写目标」（Qt 在目录不可写等场景默认回落），否则
  // 原子性承诺失效。
  f.setDirectWriteFallback( false );
  if ( !f.open( QIODevice::WriteOnly ) )
    return fail( QStringLiteral( "无法写入 %1：%2" ).arg( path, f.errorString() ) );
  if ( f.write( bytes ) != bytes.size() )
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    return fail( QStringLiteral( "写入不完整 %1：%2" ).arg( path, detail ) );
  }
  if ( !f.commit() )
    return fail( QStringLiteral( "提交失败 %1：%2" ).arg( path, f.errorString() ) );
  return true;
}

/// 原子写 JSON 文档（同上语义）。
inline bool writeJsonAtomic( const QString &path, const QJsonDocument &doc,
                             QString *error = nullptr,
                             QJsonDocument::JsonFormat format = QJsonDocument::Indented,
                             bool mkdirs = false )
{
  return writeFileAtomic( path, doc.toJson( format ), error, mkdirs );
}

} // namespace paleo
