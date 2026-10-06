// 层：视图
#include "shortcutsheetdialog.h"

#include "../paleotheme.h"
#include "shortcutcatalog.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace paleo::shortcuts
{

namespace
{
constexpr int kIdRole = Qt::UserRole;      // 条目行：登记 id；分组行：空
constexpr int kContextRole = Qt::UserRole + 1; // 条目行：作用域路径
constexpr int kSearchRole = Qt::UserRole + 2;  // 条目行：检索文本（小写拼接）
} // namespace

ShortcutSheetDialog::ShortcutSheetDialog(const ShortcutRegistry &registry, QWidget *parent)
  : QDialog(parent), m_registry(registry)
{
  setObjectName(QStringLiteral("shortcutSheetDialog"));
  setWindowTitle(tr("快捷键总表"));
  setModal(false);
  resize(680, 540);

  const auto &tk = PaleoTheme::tokens();
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(tk.spacingMd, tk.spacingMd, tk.spacingMd, tk.spacingMd);
  lay->setSpacing(tk.spacingSm);

  m_search = new QLineEdit(this);
  m_search->setObjectName(QStringLiteral("shortcutSearchEdit"));
  m_search->setPlaceholderText(tr("搜索按键、功能或面板…"));
  m_search->setAccessibleName(tr("搜索快捷键"));
  m_search->setClearButtonEnabled(true);
  lay->addWidget(m_search);

  m_contextOnly = new QCheckBox(tr("只看当前位置可用的条目"), this);
  m_contextOnly->setObjectName(QStringLiteral("shortcutContextOnlyCheck"));
  m_contextOnly->setToolTip(tr("依据按 F1 时键盘焦点所在的面板过滤"));
  m_contextOnly->hide(); // 有「当前位置」时才显示
  lay->addWidget(m_contextOnly);

  m_tree = new QTreeWidget(this);
  m_tree->setObjectName(QStringLiteral("shortcutTree"));
  m_tree->setAccessibleName(tr("快捷键列表"));
  m_tree->setColumnCount(3);
  m_tree->setHeaderLabels({tr("按键"), tr("功能"), tr("适用范围")});
  m_tree->setRootIsDecorated(false);
  m_tree->setUniformRowHeights(true);
  m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_tree->header()->setStretchLastSection(false);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  lay->addWidget(m_tree, 1);

  auto *footer = new QHBoxLayout;
  footer->setSpacing(tk.spacingSm);
  m_count = new QLabel(this);
  m_count->setObjectName(QStringLiteral("shortcutCountLabel"));
  PaleoTheme::applyThemedStyleSheet(m_count, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  footer->addWidget(m_count, 1);
  m_copy = new QPushButton(tr("复制所选"), this);
  m_copy->setObjectName(QStringLiteral("shortcutCopyButton"));
  m_copy->setToolTip(tr("把所选条目以「按键、功能、适用范围」复制到剪贴板"));
  m_copy->setEnabled(false);
  footer->addWidget(m_copy);
  lay->addLayout(footer);

  m_hint = new QLabel(this);
  m_hint->setObjectName(QStringLiteral("shortcutWhatsThisHint"));
  m_hint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  const QKeySequence whatsThisKey = m_registry.key(QStringLiteral("main.help.whatsThis"));
  if (whatsThisKey.isEmpty())
    m_hint->hide();
  else
    m_hint->setText(tr("提示：按 %1 进入「这是什么？」模式，再点击界面上的控件即可查看它的说明。")
                        .arg(displayKey(whatsThisKey)));
  lay->addWidget(m_hint);

  auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(box);

  connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
  connect(m_contextOnly, &QCheckBox::toggled, this, [this] { applyFilter(); });
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
          [this] { m_copy->setEnabled(!selectedEntryId().isEmpty()); });
  connect(m_copy, &QPushButton::clicked, this, [this] { copySelected(); });
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item || item->data(0, kIdRole).toString().isEmpty())
      return;
    m_tree->setCurrentItem(item);
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("复制此条"));
    if (menu.exec(m_tree->viewport()->mapToGlobal(pos)) == copy)
      copySelected();
  });
  // 总表自身的复制键也走中央注册表（dialog/shortcut-sheet 作用域）。
  QShortcut *copyKey = bindShortcut(QStringLiteral("sheet.copy"), this);
  copyKey->setObjectName(QStringLiteral("shortcutSheetCopyShortcut"));
  connect(copyKey, &QShortcut::activated, this, [this] { copySelected(); });

  rebuild();
}

void ShortcutSheetDialog::rebuild()
{
  m_tree->clear();
  QFont groupFont = m_tree->font();
  groupFont.setWeight(QFont::DemiBold);
  const QFont mono = PaleoTheme::monoFont();
  for (const QString &group : m_registry.groups())
  {
    auto *groupItem = new QTreeWidgetItem(m_tree);
    groupItem->setText(0, displayGroup(group));
    groupItem->setFont(0, groupFont);
    groupItem->setFlags(Qt::ItemIsEnabled);
    groupItem->setFirstColumnSpanned(true);
    for (const ShortcutEntry &e : m_registry.entriesInGroup(group))
    {
      auto *item = new QTreeWidgetItem(groupItem);
      const QString keyText = displayKey(e.key);
      const QString description = displayDescription(e);
      const QString scope = displaySourcePanel(e);
      item->setText(0, keyText);
      item->setFont(0, mono);
      item->setText(1, description);
      item->setText(2, scope);
      item->setToolTip(1, description);
      item->setData(0, kIdRole, e.id);
      item->setData(0, kContextRole, e.context);
      item->setData(0, kSearchRole,
                    QStringList{keyText, e.key.toString(QKeySequence::PortableText), description,
                                scope, displayGroup(group)}
                        .join(QLatin1Char('\n'))
                        .toLower());
    }
  }
  m_tree->expandAll();
  applyFilter();
}

void ShortcutSheetDialog::applyFilter()
{
  const QString needle = m_search->text().trimmed().toLower();
  const bool contextOnly = m_contextOnly->isChecked() && !m_activeContext.isEmpty();
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g)
  {
    QTreeWidgetItem *groupItem = m_tree->topLevelItem(g);
    int visibleChildren = 0;
    for (int c = 0; c < groupItem->childCount(); ++c)
    {
      QTreeWidgetItem *item = groupItem->child(c);
      bool show = needle.isEmpty() || item->data(0, kSearchRole).toString().contains(needle);
      if (show && contextOnly)
        show = ShortcutRegistry::contextsRelated(item->data(0, kContextRole).toString(),
                                                 m_activeContext);
      item->setHidden(!show);
      if (show)
        ++visibleChildren;
    }
    groupItem->setHidden(visibleChildren == 0);
  }
  updateCountLabel();
}

void ShortcutSheetDialog::updateCountLabel()
{
  const int total = entryCount();
  const int visible = visibleEntryCount();
  if (visible == total)
    m_count->setText(tr("共 %n 条", nullptr, total));
  else if (visible == 0)
    m_count->setText(tr("没有匹配的条目"));
  else
    m_count->setText(tr("显示 %1 / %2 条").arg(visible).arg(total));
}

int ShortcutSheetDialog::entryCount() const
{
  int n = 0;
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g)
    n += m_tree->topLevelItem(g)->childCount();
  return n;
}

int ShortcutSheetDialog::visibleEntryCount() const
{
  int n = 0;
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g)
  {
    const QTreeWidgetItem *groupItem = m_tree->topLevelItem(g);
    for (int c = 0; c < groupItem->childCount(); ++c)
      if (!groupItem->child(c)->isHidden())
        ++n;
  }
  return n;
}

void ShortcutSheetDialog::setFilterText(const QString &text)
{
  m_search->setText(text); // textChanged → applyFilter
}

QString ShortcutSheetDialog::filterText() const
{
  return m_search->text();
}

void ShortcutSheetDialog::setActiveContext(const QString &context)
{
  m_activeContext = context;
  m_contextOnly->setVisible(!context.isEmpty());
  if (context.isEmpty())
  {
    m_contextOnly->setChecked(false);
    applyFilter();
    return;
  }
  applyFilter();
  // 定位到「离当前位置最近」的条目：覆盖该位置的最深上下文优先，其次是
  // 该位置下的子作用域（焦点内按键）——与注册表的上下文优先级同向。
  QString bestId;
  int bestScore = -1;
  for (const ShortcutEntry &e : m_registry.entries())
  {
    if (!ShortcutRegistry::contextsRelated(e.context, context))
      continue;
    const int depth = ShortcutRegistry::contextDepth(e.context);
    const int score = ShortcutRegistry::contextCovers(e.context, context) ? 100 + depth : depth;
    if (score > bestScore)
    {
      bestScore = score;
      bestId = e.id;
    }
  }
  if (!bestId.isEmpty())
    selectEntry(bestId);
}

void ShortcutSheetDialog::setContextFilterEnabled(bool on)
{
  m_contextOnly->setChecked(on);
}

QTreeWidgetItem *ShortcutSheetDialog::itemFor(const QString &entryId) const
{
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if ((*it)->data(0, kIdRole).toString() == entryId)
      return *it;
  return nullptr;
}

QString ShortcutSheetDialog::clipboardTextFor(const QString &entryId) const
{
  const QTreeWidgetItem *item = itemFor(entryId);
  if (!item)
    return QString();
  return QStringList{item->text(0), item->text(1), item->text(2)}.join(QLatin1Char('\t'));
}

QString ShortcutSheetDialog::copySelected()
{
  const QString text = clipboardTextFor(selectedEntryId());
  if (!text.isEmpty())
    if (QClipboard *cb = QGuiApplication::clipboard())
      cb->setText(text);
  return text;
}

bool ShortcutSheetDialog::selectEntry(const QString &entryId)
{
  QTreeWidgetItem *item = itemFor(entryId);
  if (!item)
    return false;
  m_tree->setCurrentItem(item);
  m_tree->scrollToItem(item);
  return true;
}

QString ShortcutSheetDialog::selectedEntryId() const
{
  const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
  return sel.isEmpty() ? QString() : sel.front()->data(0, kIdRole).toString();
}

} // namespace paleo::shortcuts
