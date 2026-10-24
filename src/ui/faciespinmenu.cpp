// 层：视图
#include "faciespinmenu.h"
#include "../domain/facieshierarchy.h"
#include "../domain/faciestaxonomy.h"
#include <QAction>
#include <QCoreApplication>
#include <QMenu>

namespace FaciesPinMenu {
QMenu *add(QMenu *menu, const QVariantList &schema, const QVariant &faciesCode, const QVariantMap &pins,
           const Apply &apply) {
  // 与 pinCandidates / 渲染器同口径：旧冻结分类先迁进三级体系，固定键才对得上。
  QVariantList migrated;
  const auto effective = FaciesTaxonomy::upgrade(schema, &migrated) ? migrated : schema;
  // FaciesCatalog::find 对未知代码回退为「未归类」占位行，不能作为固定目标。
  QVariantMap f;
  for (const auto &row : effective)
    if (!faciesCode.isNull() && row.toMap().value("code").toString() == faciesCode.toString())
      f = row.toMap();
  if (!menu || f.isEmpty())
    return nullptr;
  const auto levels = FaciesHierarchy::levels();
  const auto parts = FaciesHierarchy::path(f);
  // 最近（最深）的已固定祖先决定该相面的显示，与 pinnedLevel() 同序。
  QString pinnedKey;
  for (int i = int(levels.size()) - 1; i >= 0 && pinnedKey.isEmpty(); --i)
    if (pins.contains(FaciesHierarchy::key(f, levels.at(i))))
      pinnedKey = FaciesHierarchy::key(f, levels.at(i));
  auto *sub = menu->addMenu(QCoreApplication::translate("FaciesPinMenu", "固定显示层级"));
  sub->setObjectName("pinFeatureFaciesLevel");
  for (int i = 0; i < int(levels.size()); ++i) {
    const auto level = levels.at(i);
    // 已有固定作用于此相面时改该固定的层级（否则更浅的新固定会被最近一级盖住、
    // 选了看不到效果）；尚无固定时固定该相面在所选层级上的节点。
    const auto key = pinnedKey.isEmpty() ? FaciesHierarchy::key(f, level) : pinnedKey;
    auto *action = sub->addAction(QCoreApplication::translate("FaciesPinMenu", "%1（%2）")
                                      .arg(FaciesHierarchy::title(level),
                                           parts.mid(0, i + 1).join(QStringLiteral(" / "))));
    action->setObjectName("pinFacies_" + level);
    action->setCheckable(true);
    action->setChecked(!pinnedKey.isEmpty() && pins.value(pinnedKey).toString() == level);
    QObject::connect(action, &QAction::triggered, sub, [apply, key, level] { apply(key, level); });
  }
  if (!pinnedKey.isEmpty()) {
    sub->addSeparator();
    auto *unpin = sub->addAction(QCoreApplication::translate("FaciesPinMenu", "取消固定"));
    unpin->setObjectName("unpinFacies");
    unpin->setToolTip(QCoreApplication::translate("FaciesPinMenu", "移除作用于此相面的最近一级固定"));
    QObject::connect(unpin, &QAction::triggered, sub,
                     [apply, pinnedKey] { apply(pinnedKey, QStringLiteral("auto")); });
  }
  return sub;
}
} // namespace FaciesPinMenu
