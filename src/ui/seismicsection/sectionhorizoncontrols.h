// 层：视图
#pragma once
#include "domain/seismic/sectionhorizons.h"
#include <QToolButton>

namespace seismic {
class SectionHorizonButton : public QToolButton {
public:
  explicit SectionHorizonButton(QWidget *parent=nullptr):QToolButton(parent){
    setObjectName(QStringLiteral("sectionHorizonToggle"));
    setText(QObject::tr("地震层位"));setAccessibleName(QObject::tr("叠加地震层位"));
    setCheckable(true);setAutoRaise(true);setOverlayStatus({});
  }
  void setOverlayStatus(const SectionHorizons& overlay){
    QString text=QObject::tr("叠加大层界面及 S1–S4 小层分界；虚线为方案推导分界，实线为已解释界线。");
    text+=QLatin1Char('\n');
    text+=overlay.curves.empty()?QObject::tr("当前剖面没有可叠加层位。"):QObject::tr("当前剖面：%1 条界线。").arg(overlay.curves.size());
    if(!overlay.warnings.isEmpty())text+=QLatin1Char('\n')+overlay.warnings.join(QLatin1Char('\n'));
    setToolTip(text);
  }
};
}
