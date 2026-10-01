#pragma once
#include <QDir>
#include <QString>
#include <QTest>
#include <QWidget>

#include "../src/ui/paleotheme.h"

// goal/ui-experience-polish — 面板修前/修后截图证据的共享出口。
// 环境变量 PALEO_UI_CAPTURE=<dir> 时把面板渲染成 <dir>/<name>_{light,dark}.png；
// 未设或值为 "0"（perf 型测试的防御哨兵）时零开销直通（不 show、不切主题）。
// 与 tst_ui/tst_uxtheme/tst_wellcomposite 既有 PALEO_UI_CAPTURE 门控同一惯例，
// 只是收敛成共享实现供各面板测试复用。
namespace uipolish
{
  inline bool captureEnabled( QString *dirOut = nullptr )
  {
    const QString dir = qEnvironmentVariable( "PALEO_UI_CAPTURE" );
    const bool on = !dir.isEmpty() && dir != QLatin1String( "0" );
    if ( on && dirOut )
      *dirOut = dir;
    return on;
  }

  inline void capturePanel( QWidget *panel, const QString &name, QSize size = QSize( 520, 360 ) )
  {
    QString dir;
    if ( !captureEnabled( &dir ) || !panel )
      return;
    QDir().mkpath( dir );
    panel->resize( size );
    panel->show();
    QTest::qWait( 40 );
    panel->grab().save( dir + QLatin1Char( '/' ) + name + QStringLiteral( "_light.png" ) );
    // 暗色证据：token 化前硬编码浅色面板在暗色下可读性崩坏——修前/修后
    // 差异主要看暗色档。
    PaleoTheme::applyDarkTheme();
    QTest::qWait( 20 );
    panel->grab().save( dir + QLatin1Char( '/' ) + name + QStringLiteral( "_dark.png" ) );
    PaleoTheme::applyLightTheme();
    panel->hide();
  }
} // namespace uipolish
