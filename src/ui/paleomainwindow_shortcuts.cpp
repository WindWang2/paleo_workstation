// 层：视图
// paleomainwindow_shortcuts — 页快捷键域（方向 63 注册表 / 方向 83 自成 TU）：
// Ctrl+1..N 直切工作流 ribbon 页（N = kRibbonIds.size()，现为 5）+ Ctrl+Tab / Ctrl+Shift+Tab 循环切中央视图页签。键序唯一
// 真源在 shortcuts/shortcutcatalog（main.page.<页 id> / main.page.next/prev）；
// buildRibbon 在页签创建后、tab 切换接线前调用（原行序）。
#include "paleomainwindow.h"

#include "pages/pageshared.h"        // kPageIds（页序 = 工作流链序）
#include "shortcuts/shortcutcatalog.h" // 方向63：快捷键中央注册表

#include <QShortcut>
#include <QTabBar>

void PaleoMainWindow::registerPageShortcuts()
{
  // W5 键盘可达：Ctrl+1..N 直切工作流 ribbon 页（N = kRibbonIds.size()；页序 = 工作流链序）。
  // 方向63：键序登记在 shortcuts/shortcutcatalog（main.page.<页 id>）。
  for (int i = 0; i < paleo::pagesinternal::kRibbonIds.size(); ++i)
  {
    auto *sc = paleo::shortcuts::bindShortcut(
        QStringLiteral("main.page.") + paleo::pagesinternal::kRibbonIds.at(i), this);
    sc->setObjectName(QStringLiteral("pageShortcut.") + paleo::pagesinternal::kRibbonIds.at(i));
    connect(sc, &QShortcut::activated, this, [this, i] {
      ribbonBar()->raiseCategory(categoryForPage(paleo::pagesinternal::kRibbonIds.at(i)));
    });
  }
  // goal/ui-experience-polish：Ctrl+Tab / Ctrl+Shift+Tab 循环切中央视图页签
  // （m_visualizationTabs，桌面页签惯例）；Ctrl+1..N 切的是 ribbon 工作流页，二者不是同一对象。
  {
    const auto cyclePage = [this](int step) {
      if (!m_visualizationTabs || m_visualizationTabs->count() == 0) return;
      const int n = m_visualizationTabs->count();
      m_visualizationTabs->setCurrentIndex((m_visualizationTabs->currentIndex() + step + n) % n);
    };
    auto *nextSc = paleo::shortcuts::bindShortcut(QStringLiteral("main.page.next"), this);
    nextSc->setObjectName(QStringLiteral("pageShortcut.next"));
    connect(nextSc, &QShortcut::activated, this, [cyclePage] { cyclePage(1); });
    auto *prevSc = paleo::shortcuts::bindShortcut(QStringLiteral("main.page.prev"), this);
    prevSc->setObjectName(QStringLiteral("pageShortcut.prev"));
    connect(prevSc, &QShortcut::activated, this, [cyclePage] { cyclePage(-1); });
  }
}
