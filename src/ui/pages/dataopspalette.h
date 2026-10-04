// 层：视图
// ui/pages/dataopspalette — D6 键盘与命令面板。
//   · DataCommandPalette：Ctrl+Shift+P 模糊搜资产/实体/动作/过滤器，回车执行
//   · ShortcutsDialog：快捷键表（? 键或帮助菜单）
//   · 冲突提示（D6.6，检测在 CommandRegistry::shortcutConflicts）
#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QShortcut>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "../paleotheme.h"
#include "dataops/dataopsfuzzy.h"

namespace paleo::dataops
{

// ---- D6.1 命令面板 ------------------------------------------------------------
// 面板条目三种来源：命令注册表（动作/过滤器预设）、资产、实体——统一走
// 模糊打分排序，回车执行当前条目。
class DataCommandPalette : public QDialog
{
  Q_OBJECT

public:
  struct Source
  {
    QString kind;     // "command" | "asset" | "entity"
    QString id;       // 命令 id / 资产 id / 实体 id
    QString title;    // 显示行
    QString subtitle; // 次级说明（类型/分类）
    int weight = 0;
    std::function<void()> trigger; // command 才有
  };

  explicit DataCommandPalette(QWidget *parent = nullptr)
    : QDialog(parent, Qt::Tool)
  {
    setObjectName(QStringLiteral("dataCommandPalette"));
    setWindowTitle(tr("命令面板"));
    setModal(true);
    resize(480, 360);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
    lay->setSpacing(PaleoTheme::tokens().spacingSm);
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("paletteInput"));
    m_edit->setPlaceholderText(tr("搜索资产 / 实体 / 动作 / 过滤器…"));
    m_edit->setClearButtonEnabled(true);
    lay->addWidget(m_edit);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("paletteResults"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(m_list, 1);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("paletteStatus"));
    PaleoTheme::applyThemedStyleSheet(m_status,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_status);
    connect(m_edit, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(m_edit, &QLineEdit::returnPressed, this, &DataCommandPalette::activateCurrent);
    connect(m_list, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *) { activateCurrent(); });
    // 面板键盘：↑↓ 选条目、Esc 关闭（QDialog 自带 reject）。
    auto *up = new QShortcut(Qt::Key_Up, this);
    auto *down = new QShortcut(Qt::Key_Down, this);
    connect(up, &QShortcut::activated, this, [this] { moveSelection(-1); });
    connect(down, &QShortcut::activated, this, [this] { moveSelection(+1); });
  }

  void setSources(QVector<Source> sources)
  {
    m_sources = std::move(sources);
    refill();
  }

  QListWidget *resultList() const { return m_list; }

signals:
  // 非 command 条目（资产/实体）执行 = 请求定位（DataPage 接线）。
  void assetChosen(const QString &assetId); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void entityChosen(const QString &entityId); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void commandChosen(const QString &commandId); // NOLINT(readability-inconsistent-declaration-parameter-name)

public slots:
  void activateCurrent()
  {
    QListWidgetItem *it = m_list->currentItem();
    if (!it)
      return;
    const int idx = it->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_visible.size())
      return;
    const Source &s = m_visible.at(idx);
    if (s.kind == QLatin1String("command"))
    {
      if (s.trigger)
        s.trigger();
      emit commandChosen(s.id);
    }
    else if (s.kind == QLatin1String("asset"))
      emit assetChosen(s.id);
    else if (s.kind == QLatin1String("entity"))
      emit entityChosen(s.id);
    accept();
  }

private:
  void moveSelection(int delta)
  {
    if (m_list->count() == 0)
      return;
    int row = m_list->currentRow() + delta;
    if (row < 0)
      row = m_list->count() - 1;
    if (row >= m_list->count())
      row = 0;
    m_list->setCurrentRow(row);
  }

  void refill()
  {
    m_list->clear();
    m_visible.clear();
    const QString q = m_edit->text().trimmed();
    // 候选打分：title 主、subtitle 次。
    struct Scored
    {
      int score;
      int srcIndex;
    };
    QVector<Scored> scored;
    for (int i = 0; i < m_sources.size(); ++i)
    {
      const Source &s = m_sources.at(i);
      int sc = fuzzyScore(s.title, q);
      if (!s.subtitle.isEmpty())
        sc = qMax(sc, fuzzyScore(s.subtitle, q) / 2);
      if (sc <= 0)
        continue;
      scored.append({sc + s.weight, i});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored &a, const Scored &b) { return a.score > b.score; });
    for (const Scored &s : scored)
    {
      const Source &src = m_sources.at(s.srcIndex);
      auto *it = new QListWidgetItem(src.title, m_list);
      if (!src.subtitle.isEmpty())
        it->setText(QStringLiteral("%1  ·  %2").arg(src.title, src.subtitle));
      it->setData(Qt::UserRole, m_visible.size());
      m_visible.append(src);
      if (m_list->count() == 1)
        m_list->setCurrentItem(it);
    }
    m_status->setText(m_list->count() == 0
                          ? tr("无匹配 — 换个关键词试试")
                          : tr("%1 条结果（↑↓ 选择 · 回车执行）").arg(m_list->count()));
  }

  QLineEdit *m_edit = nullptr;
  QListWidget *m_list = nullptr;
  QLabel *m_status = nullptr;
  QVector<Source> m_sources;
  QVector<Source> m_visible;
};

// ---- D6.3 快捷键表对话框 -------------------------------------------------------
// 数据源 = CommandRegistry（D6.2：动作/快捷键的统一登记处）。
class ShortcutsDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ShortcutsDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("shortcutsDialog"));
    setWindowTitle(tr("快捷键表"));
    setModal(true);
    resize(520, 420);
    auto *lay = new QVBoxLayout(this);
    m_table = new QTableWidget(0, 3, this);
    m_table->setObjectName(QStringLiteral("shortcutsTable"));
    m_table->setHorizontalHeaderLabels({tr("操作"), tr("快捷键"), tr("分类")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_table, 1);
    m_conflicts = new QLabel(this);
    m_conflicts->setObjectName(QStringLiteral("shortcutConflictsLabel"));
    m_conflicts->setWordWrap(true);
    m_conflicts->hide();
    PaleoTheme::applyThemedStyleSheet(m_conflicts, [] {
      return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; padding: {spacing.xs}px;"))
          .arg(PaleoTheme::tokens().errorText.name().toUpper());
    });
    lay->addWidget(m_conflicts);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }

  void loadRegistry(const CommandRegistry &reg)
  {
    m_table->setRowCount(0);
    for (const CommandEntry &e : reg.all())
    {
      const int r = m_table->rowCount();
      m_table->insertRow(r);
      auto *t = new QTableWidgetItem(e.title);
      t->setFlags(t->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 0, t);
      auto *k = new QTableWidgetItem(e.shortcut);
      k->setFlags(k->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 1, k);
      auto *c = new QTableWidgetItem(e.category);
      c->setFlags(c->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 2, c);
    }
    // D6.6 冲突提示。
    const QList<CommandRegistry::Conflict> conflicts = reg.shortcutConflicts();
    if (conflicts.isEmpty())
      m_conflicts->hide();
    else
    {
      QStringList parts;
      for (const auto &c : conflicts)
        parts << QStringLiteral("%1 → %2").arg(c.shortcut, c.commandIds.join(QStringLiteral(", ")));
      m_conflicts->setText(tr("快捷键冲突：%1").arg(parts.join(QStringLiteral("；"))));
      m_conflicts->show();
    }
  }

private:
  QTableWidget *m_table = nullptr;
  QLabel *m_conflicts = nullptr;
};

} // namespace paleo::dataops
