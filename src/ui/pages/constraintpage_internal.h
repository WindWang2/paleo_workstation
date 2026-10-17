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

// 解析等值级别文本。dropped（可空）回传被丢弃的非法 token 数——
// 「10, abc, 30」解析为 [10,30] 并报 dropped=1，调用方据此提示用户，
// 不静默吞掉脏输入。
// #149 对偶缺口：固定级别路径的级别数上限——粘贴数万级会让 GDAL 逐级
// 追线冻结/OOM。与 paleo::singlefactor::kMaxContourLevels
//（src/algorithms/singlefactor/contourlevels.h，工作流层闸同一来源）
// 同值对齐；视图层按层契约禁 include algorithms/*，故此处独立定义，
// 改值需两处同步。超限的多余级别按 dropped 计（对齐脏 token 机制），
// 截断保留前 N 条，调用方如实提示。
inline constexpr int kMaxContourLevelTokens = 10000;

inline QVector<double> parseLevels( const QString &text, int *dropped = nullptr )
{
  QVector<double> levels;
  if ( dropped )
    *dropped = 0;
  const QStringList parts = text.split( QRegularExpression( QStringLiteral( "[,，\\s]+" ) ), Qt::SkipEmptyParts );
  for ( const QString &part : parts )
  {
    bool ok = false;
    const double value = part.toDouble( &ok );
    if ( !ok )
    {
      if ( dropped )
        ++( *dropped );
      continue;
    }
    if ( levels.size() >= kMaxContourLevelTokens )
    {
      // 有效但超出级别数上限：与前 kMaxContourLevelTokens 条一样按丢弃
      // 计数上报——不静默截断（与工作流层「级别数超过上限」闸对偶）。
      if ( dropped )
        ++( *dropped );
      continue;
    }
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

// takeAt/addItem 不会替控件改 parent；跨容器搬迁时显式交给目标布局，
// 否则旧父控件仍按旧坐标绘制，标题会叠到页首、折叠也无法隐藏。
inline void appendMovedItem(QVBoxLayout *target, QLayoutItem *item)
{
  if (auto *widget = item->widget())
  {
    target->addWidget(widget, 0, item->alignment());
    delete item;
  }
  else if (auto *layout = item->layout())
  {
    layout->setParent(nullptr);
    target->addLayout(layout);
  }
  else
    target->addItem(item);
}

} // namespace paleo::constraintpage_internal
