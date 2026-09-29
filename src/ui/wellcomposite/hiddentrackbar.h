// 层：视图
#pragma once

#include <QString>
#include <QToolButton>
#include <QWidget>

#include <QList>

#include "trackregistry.h"

// ui/wellcomposite/hiddentrackbar — D1.10 隐藏道管理条
//
// 画布底部的 chip 列：列出当前被隐藏的道（标题 chip），点击恢复显示。
// 无隐藏道时整条自动隐藏（不占布局空间——QSizePolicy::Fixed + hide）。

namespace WellComposite
{

class HiddenTrackBar : public QWidget
{
  Q_OBJECT

public:
  explicit HiddenTrackBar(QWidget *parent = nullptr);

  // 以 spec 列表驱动（仅显示 visible=false 的项）
  void setTracks(const QList<TrackSpec> &specs);
  int hiddenCount() const;

signals:
  // 用户点击某隐藏道 chip 请求恢复其显示
  void trackRestoreRequested(const QString &trackTitle);

private:
  void rebuild();

  QList<TrackSpec> m_specs;
};

} // namespace WellComposite
