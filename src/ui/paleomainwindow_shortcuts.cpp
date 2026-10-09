// 层：视图
// paleomainwindow_shortcuts — 页快捷键域（方向 63 注册表 / 方向 83 自成 TU）：
// Ctrl+1..6 直切六工作流页 + Ctrl+Tab / Ctrl+Shift+Tab 循环切页。键序唯一
// 真源在 shortcuts/shortcutcatalog（main.page.<页 id> / main.page.next/prev）；
// buildRibbon 在页签创建后、tab 切换接线前调用（原行序）。
#include "paleomainwindow.h"

#include "pages/pageshared.h"        // kPageIds（页序 = 工作流链序）
#include "shortcuts/shortcutcatalog.h" // 方向63：快捷键中央注册表

#include <QShortcut>

void PaleoMainWindow::registerPageShortcuts()
{
  // W5 键盘可达：Ctrl+1..6 直切六个工作流页（页序 = 工作流链序）。
  // 方向63：键序登记在 shortcuts/shortcutcatalog（main.page.<页 id>）。
  for (int i = 0; i < paleo::pagesinternal::kPageIds.size(); ++i)
  {
    auto *sc = paleo::shortcuts::bindShortcut(
        QStringLiteral("main.page.") + paleo::pagesinternal::kPageIds.at(i), this);
    sc->setObjectName(QStringLiteral("pageShortcut.") + paleo::pagesinternal::kPageIds.at(i));
    connect(sc, &QShortcut::activated, this, [this, i] {
      showPage(paleo::pagesinternal::kPageIds.at(i));
    });
  }
  // goal/ui-experience-polish：Ctrl+Tab / Ctrl+Shift+Tab 循环切页（桌面页签
  // 惯例；与 Ctrl+1..6 互补——手不离开主行也能走完整工作流链）。
  {
    const auto cyclePage = [this](int step) {
      const int idx = paleo::pagesinternal::kPageIds.indexOf(m_currentPage);
      const int n = paleo::pagesinternal::kPageIds.size();
      const int next = ((idx < 0 ? 0 : idx) + step + n) % n;
      showPage(paleo::pagesinternal::kPageIds.at(next));
    };
    auto *nextSc = paleo::shortcuts::bindShortcut(QStringLiteral("main.page.next"), this);
    nextSc->setObjectName(QStringLiteral("pageShortcut.next"));
    connect(nextSc, &QShortcut::activated, this, [cyclePage] { cyclePage(1); });
    auto *prevSc = paleo::shortcuts::bindShortcut(QStringLiteral("main.page.prev"), this);
    prevSc->setObjectName(QStringLiteral("pageShortcut.prev"));
    connect(prevSc, &QShortcut::activated, this, [cyclePage] { cyclePage(-1); });
  }
}
