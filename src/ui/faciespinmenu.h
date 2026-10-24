// 层：视图
#pragma once
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
class QMenu;
// 画布右键「固定显示层级」子菜单：按被点相面的相类别代码，尚无固定时在其相 /
// 亚相 / 微相节点上固定显示为同名层级；已有固定作用于该相面时，选层级即改该
// （最近一级）固定的层级，并追加「取消固定」。
// 只收集意图（apply(nodeKey, mode)，mode 为 "auto" 表示移除），不改图层。
namespace FaciesPinMenu {
using Apply = std::function<void(const QString &nodeKey, const QString &mode)>;
// faciesCode 为空或不在分类中（未预测 / 未归类）时不加子菜单并返回 nullptr。
QMenu *add(QMenu *menu, const QVariantList &schema, const QVariant &faciesCode, const QVariantMap &pins,
           const Apply &apply);
} // namespace FaciesPinMenu
