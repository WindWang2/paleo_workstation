// 层：功能
#pragma once

#include <QString>
#include <QStringList>

#include <QObject>
#include <QtGlobal>

#include "../services/errorhub.h"

namespace paleo::workflow_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError( QString *error, const QString &text )
{
  if ( error )
    *error = text;
}

/// #556 吞错路径收口：调用点 qWarning 保留为诊断日志，同时接 ErrorHub
/// raise() 让用户/任务面板可见（DESIGN.md 错误呈现 error/warning 档：
/// 非模态通知卡 + 环形历史）。raise 任意线程可调；未安装全局 hub
/// （单测/组装根之前）为 no-op——调用点自身的 qWarning 已兜底诊断。
inline void raiseToErrorHub( ErrorHub::Level level, const QString &source,
                             const QString &title, const QString &text )
{
  if ( ErrorHub *hub = ErrorHub::global() )
    hub->raise( level, source, title, text );
}

/// 版面导出快照重建的缺层警告统一入账（DESIGN.md 错误呈现 warning 档：
/// 非模态通知卡 + 环形历史）。ErrorHub::raise 任意线程可调——worker 线程
/// 直接上报，errorRaised 经 AutoConnection 排队回 GUI 线程呈现；未安装
/// 全局 hub（单测/组装根之前）降级 stderr，不静默。
inline void reportSnapshotWarnings( const QStringList &warnings )
{
  if ( warnings.isEmpty() )
    return;
  const QString text = warnings.join( QLatin1Char( '\n' ) );
  if ( ErrorHub *hub = ErrorHub::global() )
    hub->raise( ErrorHub::Level::Warning, QStringLiteral( "layoutexport" ),
                QObject::tr( "导出快照有 %1 个图层无法加载，产物不含这些图层" )
                    .arg( warnings.size() ),
                text );
  else
    qWarning( "paleo layoutexport snapshot warnings: %s", qPrintable( text ) );
}

} // namespace paleo::workflow_detail
