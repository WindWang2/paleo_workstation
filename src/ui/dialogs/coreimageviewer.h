// 层：视图
#pragma once
#include <QString>

class QWidget;
namespace WellComposite { struct ImageDepthItem; }

namespace PaleoCoreImageViewer {
// 非模态单张检视；原图通过 imagelod 后台装载，关闭窗口后丢弃回调。
void open(const WellComposite::ImageDepthItem &item, const QString &wellName,
          QWidget *parent);
}
