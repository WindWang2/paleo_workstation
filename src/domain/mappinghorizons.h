#pragma once
#include <QString>
#include <QStringList>

// domain/ — 阶段E 的编图层位集合（docs/PROJECT_AREA_PLAN.md §1、§5E）。
// 工区有 8 个层序界面；编图 chip、厚度基面推导都以这个有序集合为准
// （浅→深）。井分层里的其余名字只出现在连井面板，不进 chip、不参与编图。
inline const QStringList &mappingHorizons()
{
  static const QStringList horizons = {
    QStringLiteral( "C3" ),  QStringLiteral( "C6" ),
    QStringLiteral( "D53" ), QStringLiteral( "D61" ),
    QStringLiteral( "D62" ), QStringLiteral( "D63" ),
    QStringLiteral( "D71" ), QStringLiteral( "D72" ),
  };
  return horizons;
}

inline bool isMappingHorizon( const QString &name )
{
  return mappingHorizons().contains( name );
}

// 下一个更深的界面 = 厚度基面（如 D61→D62）。集合外或最深层返回空。
inline QString baseHorizonFor( const QString &horizon )
{
  const QStringList &horizons = mappingHorizons();
  const int idx = horizons.indexOf( horizon );
  return ( idx >= 0 && idx + 1 < horizons.size() ) ? horizons.at( idx + 1 ) : QString();
}
