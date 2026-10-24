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

  // 返回 false 仅表示「开启了截图却没能落盘/面板为空」；未开启截图时恒为 true。
  // 调用方可 QVERIFY 返回值，但证据槽仍须另有针对面板状态的真实断言。
  inline bool capturePanel( QWidget *panel, const QString &name, QSize size = QSize( 520, 360 ) )
  {
    QString dir;
    if ( !panel )
      return false;
    if ( !captureEnabled( &dir ) )
      return true;
    QDir().mkpath( dir );
    panel->resize( size );
    panel->show();
    QTest::qWait( 40 );
    bool ok = panel->grab().save( dir + QLatin1Char( '/' ) + name + QStringLiteral( "_light.png" ) );
    // 暗色证据：token 化前硬编码浅色面板在暗色下可读性崩坏——修前/修后
    // 差异主要看暗色档。
    PaleoTheme::applyDarkTheme();
    QTest::qWait( 20 );
    ok = panel->grab().save( dir + QLatin1Char( '/' ) + name + QStringLiteral( "_dark.png" ) ) && ok;
    PaleoTheme::applyLightTheme();
    panel->hide();
    return ok;
  }
} // namespace uipolish
