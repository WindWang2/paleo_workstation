// 层：视图
// ui/pages/constraintpage_internal — 约束页家族 TU 共用的文件域助手
//（方向 96 自本体匿名 namespace 收敛成内部契约头；先例：方向 83 主窗的
// paleomainwindow_internal.h、方向 65 canvas 拆分的内部头）。只含纯查找/
// 解析助手与本页动态属性键，不含业务。
#pragma once

#include <QLabel>
#include <QRegularExpression>
#include <QString>
#include <QTableWidget>
#include <QVector>
#include <QVBoxLayout>

#include "../paleotheme.h"

class QWidget;

namespace paleo::constraintpage_internal
{

// 状态（无成员设计，走动态属性——与 pagepanels.cpp 同一惯例）：
// factorId -> 已生成 layerId。
inline constexpr const char kFactorGenProp[] = "paleo.page.factorgen";

inline QString factorIdOfRow( const QTableWidget *table, int row )
{
  if ( !table || row < 0 || row >= table->rowCount() )
    return QString();
  return table->item( row, 0 ) ? table->item( row, 0 )->data( Qt::UserRole ).toString()
                               : QString();
}

inline int checkedRow( const QTableWidget *table )
{
  if ( !table )
    return -1;
  for ( int r = 0; r < table->rowCount(); ++r )
  {
    if ( table->item( r, 0 ) &&
         table->item( r, 0 )->checkState() == Qt::Checked )
      return r;
  }
  return -1;
}

inline bool interpolantEngine( const QString &algorithmId )
{
  return algorithmId == QLatin1String( "paleo:paleo_constraint_idw" );
}

inline void useMono( QWidget *widget )
{
  if ( widget )
    widget->setFont( PaleoTheme::monoFont() );
}

inline QVector<double> parseLevels( const QString &text )
{
  QVector<double> levels;
  const QStringList parts = text.split( QRegularExpression( QStringLiteral( "[,，\\s]+" ) ), Qt::SkipEmptyParts );
  for ( const QString &part : parts )
  {
    bool ok = false;
    const double value = part.toDouble( &ok );
    if ( ok )
      levels << value;
  }
  return levels;
}

// 高级参数面 → 约束族段共用：把控件连同其前置标题从高级区布局迁入目标
// 布局（ctor 期一次成形；方向 96 自 ctor 局部 lambda 提为自由函数）。
inline void moveParamWidget( QVBoxLayout *adv, QWidget *control, QVBoxLayout *target )
{
  const int index = adv->indexOf( control );
  if ( index > 0 )
  {
    auto *previous = adv->itemAt( index - 1 )->widget();
    if ( qobject_cast<QLabel *>( previous ) )
    {
      adv->removeWidget( previous );
      target->addWidget( previous );
    }
  }
  adv->removeWidget( control );
  target->addWidget( control );
}

} // namespace paleo::constraintpage_internal
