// 层：视图
#include "timeslicereadcontrols.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QSettings>

namespace paleo::ui {
TimeSliceReadControls makeTimeSliceReadControls(QWidget *parent,QHBoxLayout *layout,QSettings *settings)
{
  auto *resolution=new QComboBox(parent);
  resolution->setObjectName(QStringLiteral("timeSliceResolution"));
  resolution->addItem(QObject::tr("快速预览（≤64×64）"),64);
  resolution->addItem(QObject::tr("快速预览（≤128×128）"),128);
  resolution->addItem(QObject::tr("完整分辨率"),0);
  resolution->setToolTip(QObject::tr("预览读取均匀抽样的真实振幅；完整分辨率按原始测网逐点读取，可取消。"));
  layout->addWidget(resolution);
  auto *workers=new QComboBox(parent);
  workers->setObjectName(QStringLiteral("timeSliceWorkers"));
  for(int count:{1,2,4})workers->addItem(QObject::tr("读取：%1 线程").arg(count),count);
  if(!settings)settings=new QSettings(parent);
  const int saved=workers->findData(settings->value(QStringLiteral("seismic/timeSliceWorkers"),4).toInt());
  workers->setCurrentIndex(saved<0?workers->findData(4):saved);
  workers->setToolTip(QObject::tr("直读 SEG-Y 的并发读取线程数；切换后重取当前切片，已有缓存直接复用。分页工作区使用自身读取调度。"));
  layout->addWidget(workers);
  QObject::connect(workers,&QComboBox::currentIndexChanged,settings,[workers,settings]{
    settings->setValue(QStringLiteral("seismic/timeSliceWorkers"),workers->currentData());
  });
  return {resolution,workers};
}
}
