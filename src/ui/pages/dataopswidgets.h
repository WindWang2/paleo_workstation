// 层：视图
// ui/pages/dataops/dataopswidgets — D1/D2 的列表侧小部件：多维过滤条、条件
// chip 行、标签云、空结果态、选中徽标、未决快捷过滤（D2.3）。
// 全部 token 走 PaleoTheme（DESIGN.md：交互蓝只做选中/主操作，徽标语义色
// 永远配文字）。
#pragma once

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLayout>
#include <QList>
#include <QRect>
#include <QWidgetItem>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include "../paleotheme.h"
#include "dataops/dataopsfilter.h"
#include "dataops/dataopsmodel.h"

namespace paleo::dataops
{

// ---- 流式布局（chip 云/标签云：超出宽度自动换行；最小宽 = 最宽单件）------
class FlowLayout : public QLayout
{
public:
  explicit FlowLayout(QWidget *parent, int margin = 0, int spacing = PaleoTheme::tokens().spacingXs)
    : QLayout(parent)
  {
    setContentsMargins(margin, margin, margin, margin);
    setSpacing(spacing);
  }
  ~FlowLayout() override
  {
    while (QLayoutItem *it = takeAt(0))
      delete it;
  }
  void addItem(QLayoutItem *item) override { m_items.append(item); }
  Qt::Orientations expandingDirections() const override { return Qt::Orientations(); }
  bool hasHeightForWidth() const override { return true; }
  int count() const override { return int(m_items.size()); }
  QLayoutItem *itemAt(int i) const override
  {
    return i >= 0 && i < m_items.size() ? m_items.at(i) : nullptr;
  }
  QSize minimumSize() const override
  {
    QSize size;
    for (const QLayoutItem *it : m_items)
      size = size.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    size += QSize(m.left() + m.right(), m.top() + m.bottom());
    return size;
  }
  QLayoutItem *takeAt(int i) override
  {
    return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr;
  }
  void setGeometry(const QRect &rect) override
  {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
  }
  QSize sizeHint() const override { return minimumSize(); }
  int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }

private:
  int doLayout(const QRect &rect, bool testOnly) const
  {
    const QMargins m = contentsMargins();
    const int left = rect.x() + m.left();
    int x = left, y = rect.y() + m.top();
    int lineHeight = 0;
    for (QLayoutItem *it : m_items)
    {
      const int space = spacing();
      const int next = x + it->sizeHint().width() + space;
      if (next - space > rect.right() - m.right() && lineHeight > 0)
      {
        x = left;
        y += lineHeight + space;
        lineHeight = 0;
      }
      if (!testOnly)
        it->setGeometry(QRect(QPoint(x, y), it->sizeHint()));
      x += it->sizeHint().width() + space;
      lineHeight = qMax(lineHeight, it->sizeHint().height());
    }
    return y + lineHeight - rect.y() + m.bottom();
  }
  QList<QLayoutItem *> m_items;
};

// ---- D1.2 选中计数徽标（面板标题右侧）--------------------------------------
class SelectionBadge : public QLabel
{
  Q_OBJECT
public:
  explicit SelectionBadge(QWidget *parent = nullptr)
    : QLabel(parent)
  {
    setObjectName(QStringLiteral("selectionBadge"));
    hide(); // 0/1 选中不占位（单选即正常态）
    PaleoTheme::applyThemedStyleSheet(this, [] {
      const auto &t = PaleoTheme::tokens();
      return PaleoTheme::metricStyleSheet(QStringLiteral("background: %1; color: %2; border: 1px solid %3;"
                            "border-radius: {rounded.md}px; padding: 0 {spacing.sm}px; font-size: {typography.label}pt;"))
          .arg(t.primary.name().toUpper(), t.onPrimary.name().toUpper(),
               t.primary.name().toUpper());
    });
  }
  void setCount(int n)
  {
    if (n <= 1)
    {
      hide();
      return;
    }
    setText(tr("已选 %1 项").arg(n));
    show();
  }
};

// ---- D2.2 条件 chip 行（每个 chip 带 × 单删）--------------------------------
class FilterChipBar : public QWidget
{
  Q_OBJECT
public:
  explicit FilterChipBar(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("filterChipBar"));
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);
    m_chips = new QWidget(this);
    m_chips->setObjectName(QStringLiteral("filterChipsHost"));
    m_chips->setMinimumWidth(0);
    m_chipsLay = new FlowLayout(m_chips, 0, PaleoTheme::tokens().spacingXs);
    lay->addWidget(m_chips, 1);
    m_clear = new QPushButton(tr("清空全部"), this);
    m_clear->setObjectName(QStringLiteral("filterClearAllButton"));
    m_clear->setFlat(true);
    m_clear->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: 0 {spacing.xs}px;")));
    connect(m_clear, &QPushButton::clicked, this, [this] { emit clearAllRequested(); });
    lay->addWidget(m_clear);
  }

  // 渲染条件集 + AND/OR 模式标记。
  void setConditions(const FilterGroup &g)
  {
    while (m_chipsLay->count() > 0)
    {
      QLayoutItem *it = m_chipsLay->takeAt(0);
      if (it->widget())
        it->widget()->deleteLater();
      delete it;
    }
    for (const FilterCondition &c : g.conditions)
    {
      auto *chip = new QPushButton(c.display(), m_chips);
      chip->setObjectName(QStringLiteral("filterChip"));
      chip->setFlat(true);
      // chip 胶囊（DESIGN.md chip token：surface 底、border 边、全圆角）。
      PaleoTheme::applyThemedStyleSheet(chip, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral("background: %1; color: %2; border: 1px solid %3;"
                              "border-radius: %4px; padding: {spacing.xs}px {spacing.md}px; font-size: {typography.label}pt;"))
            .arg(t.surface.name().toUpper(), t.textMuted.name().toUpper(),
                 t.border.name().toUpper())
            .arg(PaleoTheme::chipRadius(PaleoTheme::bodyFont(t.labelPt)));
      });
      chip->setToolTip(tr("点击删除该条件；右键取反"));
      chip->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(chip, &QPushButton::clicked, this,
              [this, c] { emit chipRemoved(c); });
      connect(chip, &QWidget::customContextMenuRequested, this,
              [this, chip, c](const QPoint &pos) {
                QMenu m(chip);
                QAction *neg = m.addAction(
                    c.negate ? tr("取消取反") : tr("取反该条件"));
                QAction *del = m.addAction(tr("删除该条件"));
                QAction *picked = m.exec(chip->mapToGlobal(pos));
                if (picked == neg)
                  emit chipToggled(c);
                else if (picked == del)
                  emit chipRemoved(c);
              });
      m_chipsLay->addWidget(chip);
    }
    setVisible(!g.conditions.isEmpty());
    m_clear->setVisible(g.conditions.size() > 1);
  }

signals:
  void chipRemoved(const paleo::dataops::FilterCondition &c); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void chipToggled(const paleo::dataops::FilterCondition &c); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void clearAllRequested();

private:
  QWidget *m_chips = nullptr;
  QLayout *m_chipsLay = nullptr;
  QPushButton *m_clear = nullptr;
};

// ---- D2.1 多维过滤条 --------------------------------------------------------
// 一行：维度下拉 + 值输入/下拉 + AND/OR 切换 + 预设下拉 + 状态串复制/粘贴。
// 值输入按维度变形：Type/Status/Role 给下拉（词表由装填方注入），其余给
// 行编辑。
class FilterBar : public QWidget
{
  Q_OBJECT
public:
  explicit FilterBar(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("multiDimFilterBar"));
    setMinimumWidth(0);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);
    auto *row1 = new QWidget(this);
    row1->setMinimumWidth(0);
    auto *r1 = new QHBoxLayout(row1);
    r1->setContentsMargins(0, 0, 0, 0);
    r1->setSpacing(PaleoTheme::tokens().spacingXs);
    auto *row2 = new QWidget(this);
    row2->setMinimumWidth(0);
    auto *r2 = new QHBoxLayout(row2);
    r2->setContentsMargins(0, 0, 0, 0);
    r2->setSpacing(PaleoTheme::tokens().spacingXs);
    lay->addWidget(row1);
    lay->addWidget(row2);

    m_dim = new QComboBox(this);
    m_dim->setObjectName(QStringLiteral("filterDimCombo"));
    m_dim->setAccessibleName(tr("过滤维度"));
    const struct
    {
      FilterDim d;
      const char *label;
    } kDims[] = {
      {FilterDim::Search, QT_TR_NOOP("搜索词")},
      {FilterDim::Type, QT_TR_NOOP("类型")},
      {FilterDim::Status, QT_TR_NOOP("状态")},
      {FilterDim::Role, QT_TR_NOOP("角色")},
      {FilterDim::Entity, QT_TR_NOOP("实体")},
      {FilterDim::Tag, QT_TR_NOOP("标签")},
      {FilterDim::Regex, QT_TR_NOOP("文件名正则")},
      {FilterDim::Unlinked, QT_TR_NOOP("未挂接")},
      {FilterDim::UnknownType, QT_TR_NOOP("类型未知")},
      {FilterDim::Warned, QT_TR_NOOP("有警告")},
    };
    for (const auto &k : kDims)
      m_dim->addItem(tr(k.label), int(k.d));
    m_dim->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_dim->setMinimumContentsLength(2);
    m_dim->setMinimumWidth(0);
    r1->addWidget(m_dim);

    m_value = new QComboBox(this);
    m_value->setObjectName(QStringLiteral("filterValueCombo"));
    m_value->setEditable(true);
    m_value->setAccessibleName(tr("过滤值"));
    m_value->setInsertPolicy(QComboBox::NoInsert);
    m_value->setMinimumWidth(30);
    m_value->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_value->setMinimumContentsLength(2);
    r1->addWidget(m_value, 1);

    m_add = new QPushButton(tr("添加"), this);
    m_add->setObjectName(QStringLiteral("filterAddButton"));
    m_add->setToolTip(tr("添加过滤条件"));
    connect(m_add, &QPushButton::clicked, this, &FilterBar::addCurrent);
    r1->addWidget(m_add);

    m_mode = new QToolButton(this);
    m_mode->setObjectName(QStringLiteral("filterModeButton"));
    m_mode->setText(tr("AND"));
    m_mode->setToolTip(tr("切换条件组合方式：AND（全部满足）/ OR（任一满足）"));
    m_mode->setCheckable(true);
    connect(m_mode, &QToolButton::toggled, this, [this](bool or_) {
      m_mode->setText(or_ ? tr("OR") : tr("AND"));
      emit modeChanged(or_);
    });
    r1->addWidget(m_mode);

    m_preset = new QComboBox(this);
    m_preset->setObjectName(QStringLiteral("filterPresetCombo"));
    m_preset->setAccessibleName(tr("保存的过滤器"));
    m_preset->addItem(tr("保存的过滤器"), QString());
    connect(m_preset, &QComboBox::activated, this, [this](int idx) {
      const QString name = m_preset->itemData(idx).toString();
      if (!name.isEmpty())
        emit presetChosen(name);
      m_preset->setCurrentIndex(0);
    });
    m_preset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_preset->setMinimumContentsLength(2);
    m_preset->setMinimumWidth(0);
    r2->addWidget(m_preset);

    auto *saveBtn = new QToolButton(this);
    saveBtn->setObjectName(QStringLiteral("filterSavePresetButton"));
    saveBtn->setText(tr("存为预设"));
    saveBtn->setToolTip(tr("把当前过滤条件存为命名预设"));
    connect(saveBtn, &QToolButton::clicked, this,
            [this] { emit savePresetRequested(); });
    r2->addWidget(saveBtn);

    auto *shareBtn = new QToolButton(this);
    shareBtn->setObjectName(QStringLiteral("filterShareButton"));
    shareBtn->setText(tr("复制状态串"));
    shareBtn->setToolTip(tr("过滤器 ↔ URL-like 状态串（可复制共享）"));
    connect(shareBtn, &QToolButton::clicked, this,
            [this] { emit shareStateRequested(); });
    r2->addWidget(shareBtn);
    r2->addStretch(1);

    // 维度切换 → 值输入形态（词表下拉 / 自由输入 / 开关型无值）。
    connect(m_dim, &QComboBox::currentIndexChanged, this, [this] { refillValueEdit(); });
    refillValueEdit();
  }

  // 值词表（类型/状态/角色/实体/标签的候选集）。
  void setValueVocabulary(FilterDim dim, const QStringList &values)
  {
    m_vocab.insert(int(dim), values);
    if (currentDim() == dim)
      refillValueEdit();
  }
  void reloadPresets()
  {
    const QString keep = m_preset->currentData().toString();
    m_preset->clear();
    m_preset->addItem(tr("保存的过滤器"), QString());
    for (const QString &n : savedFilterNames())
      m_preset->addItem(n, n);
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(keep)));
  }

  FilterDim currentDim() const
  {
    return FilterDim(m_dim->currentData().toInt());
  }
  QString currentValue() const
  {
    if (isSwitchDim(currentDim()))
      return QStringLiteral("1");
    return m_value->currentText().trimmed();
  }

  // 词表型维度：输入框选完即加（回车或点添加都行）。
  void addCurrent()
  {
    const FilterDim d = currentDim();
    QString v = currentValue();
    if (isSwitchDim(d))
      v = QStringLiteral("1");
    if (!isSwitchDim(d) && v.isEmpty())
      return;
    FilterCondition c;
    c.dim = d;
    c.value = isSwitchDim(d) ? QString() : v;
    m_value->setCurrentText(QString());
    emit conditionAdded(c);
  }

  void setOrMode(bool or_) { m_mode->setChecked(or_); }
  bool orMode() const { return m_mode->isChecked(); }

  static bool isSwitchDim(FilterDim d)
  {
    return d == FilterDim::Unlinked || d == FilterDim::UnknownType ||
           d == FilterDim::Warned;
  }

signals:
  void conditionAdded(const paleo::dataops::FilterCondition &c); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void modeChanged(bool orMode); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void presetChosen(const QString &name); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void savePresetRequested();
  void shareStateRequested();

private:
  void refillValueEdit()
  {
    const FilterDim d = currentDim();
    const QString keep = m_value->currentText();
    m_value->clear();
    if (isSwitchDim(d))
    {
      m_value->addItem(tr("（开关条件）"), QString());
      m_value->setEnabled(false);
      m_add->setEnabled(true);
      return;
    }
    m_value->setEnabled(true);
    const QStringList vocab = m_vocab.value(int(d));
    if (!vocab.isEmpty())
      for (const QString &v : vocab)
        m_value->addItem(v);
    m_value->setCurrentText(keep);
  }

  QComboBox *m_dim = nullptr;
  QComboBox *m_value = nullptr;
  QPushButton *m_add = nullptr;
  QToolButton *m_mode = nullptr;
  QComboBox *m_preset = nullptr;
  QHash<int, QStringList> m_vocab;
};

// ---- D2.3 未决项快捷过滤行（独立过滤 + 计数徽标）----------------------------
class PendingQuickBar : public QWidget
{
  Q_OBJECT
public:
  explicit PendingQuickBar(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("pendingQuickBar"));
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);
    // 宽度契约（D9）：无 caption——按钮自带文字即语义，tooltip 给细节。
    const struct
    {
      const char *name;
      const char *text;
      FilterDim dim;
    } kQuick[] = {
      {"quickUnlinked", QT_TR_NOOP("未挂接"), FilterDim::Unlinked},
      {"quickUnknownType", QT_TR_NOOP("类型未知"), FilterDim::UnknownType},
      {"quickWarned", QT_TR_NOOP("有警告"), FilterDim::Warned},
    };
    for (const auto &k : kQuick)
    {
      auto *btn = new QPushButton(tr(k.text), this);
      btn->setObjectName(QLatin1String(k.name));
      btn->setCheckable(true);
      btn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
      // 徽标文本随计数刷新（「未挂接 (12)」）。
      connect(btn, &QPushButton::toggled, this, [this, btn, d = k.dim](bool on) {
        btn->setProperty("dim", int(d));
        emit quickToggled(d, on);
      });
      m_buttons.append({btn, FilterDim(k.dim)});
      lay->addWidget(btn);
    }
    lay->addStretch(1);
  }
  // 计数徽标：三个快捷口径的命中数（装填方从快照算）。
  void setCounts(int unlinked, int unknown, int warned)
  {
    for (auto &b : m_buttons)
    {
      int n = 0;
      if (b.second == FilterDim::Unlinked) n = unlinked;
      else if (b.second == FilterDim::UnknownType) n = unknown;
      else n = warned;
      const QString base = b.first->text().section(QLatin1Char(' '), 0, 0);
      b.first->setText(base + QStringLiteral(" (%1)").arg(n));
    }
  }
  void clearAll()
  {
    for (auto &b : m_buttons)
      b.first->setChecked(false);
  }

signals:
  void quickToggled(paleo::dataops::FilterDim dim, bool on); // NOLINT(readability-inconsistent-declaration-parameter-name)

private:
  QList<QPair<QPushButton *, FilterDim>> m_buttons;
};

// ---- D2.4 标签云（可点选 = 过滤；右键 = 全删该标签）--------------------------
class TagCloudWidget : public QWidget
{
  Q_OBJECT
public:
  explicit TagCloudWidget(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("tagCloud"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);
    auto *cap = new QLabel(tr("标签"), this);
    PaleoTheme::applyThemedStyleSheet(cap,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(cap);
    m_host = new QWidget(this);
    m_host->setObjectName(QStringLiteral("tagCloudHost"));
    m_host->setMinimumWidth(0);
    m_flow = new FlowLayout(m_host, 0, PaleoTheme::tokens().spacingXs);
    lay->addWidget(m_host);
    m_empty = new QLabel(tr("尚无标签 — 选中资产后右键「打标签」"), this);
    m_empty->setObjectName(QStringLiteral("tagCloudEmpty"));
    PaleoTheme::applyThemedStyleSheet(m_empty,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_empty);
  }

  void setCloud(const QVector<QPair<QString, int>> &cloud, const QString &activeTag)
  {
    while (m_flow->count() > 0)
    {
      QLayoutItem *it = m_flow->takeAt(0);
      if (it->widget())
        it->widget()->deleteLater();
      delete it;
    }
    m_empty->setVisible(cloud.isEmpty());
    m_host->setVisible(!cloud.isEmpty());
    for (const auto &tc : cloud)
    {
      auto *chip = new QPushButton(
          QStringLiteral("%1 ×%2").arg(tc.first).arg(tc.second), m_host);
      chip->setObjectName(QStringLiteral("tagCloudChip"));
      chip->setCheckable(true);
      chip->setChecked(tc.first == activeTag);
      // 选中 chip = chip-active token（primary 底 + on-primary 字）。
      PaleoTheme::applyThemedStyleSheet(chip, [active = (tc.first == activeTag)] {
        const auto &t = PaleoTheme::tokens();
        if (active)
          return PaleoTheme::metricStyleSheet(QStringLiteral(
                     "background: %1; color: %2; border: 1px solid %1;"
                     "border-radius: %3px; padding: {spacing.xs}px {spacing.md}px; font-size: {typography.label}pt;"))
              .arg(t.primary.name().toUpper(), t.onPrimary.name().toUpper())
              .arg(PaleoTheme::chipRadius(PaleoTheme::bodyFont(t.labelPt)));
        return PaleoTheme::metricStyleSheet(QStringLiteral("background: %1; color: %2; border: 1px solid %3;"
                              "border-radius: %4px; padding: {spacing.xs}px {spacing.md}px; font-size: {typography.label}pt;"))
            .arg(t.surface.name().toUpper(), t.textMuted.name().toUpper(),
                 t.border.name().toUpper())
            .arg(PaleoTheme::chipRadius(PaleoTheme::bodyFont(t.labelPt)));
      });
      chip->setToolTip(tr("点击按此标签过滤"));
      connect(chip, &QPushButton::clicked, this,
              [this, t = tc.first, active = (tc.first == activeTag)] {
                emit tagClicked(t, !active);
              });
      m_flow->addWidget(chip);
    }
  }

signals:
  void tagClicked(const QString &tag, bool on); // NOLINT(readability-inconsistent-declaration-parameter-name)

private:
  QWidget *m_host = nullptr;
  QLayout *m_flow = nullptr;
  QLabel *m_empty = nullptr;
};

// ---- D2.9 空结果态（放宽条件快捷钮）----------------------------------------
class FilterEmptyState : public QWidget
{
  Q_OBJECT
public:
  explicit FilterEmptyState(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("filterEmptyState"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
    lay->setSpacing(PaleoTheme::tokens().spacingSm);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("filterEmptyText"));
    m_text->setWordWrap(true);
    m_text->setAlignment(Qt::AlignCenter);
    PaleoTheme::applyThemedStyleSheet(m_text,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_text);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(PaleoTheme::tokens().spacingXs);
    m_relax = new QPushButton(tr("放宽条件"), row);
    m_relax->setObjectName(QStringLiteral("filterRelaxButton"));
    connect(m_relax, &QPushButton::clicked, this, [this] { emit relaxRequested(); });
    m_clear = new QPushButton(tr("清除全部过滤"), row);
    m_clear->setObjectName(QStringLiteral("filterEmptyClearButton"));
    connect(m_clear, &QPushButton::clicked, this, [this] { emit clearRequested(); });
    rl->addStretch(1);
    rl->addWidget(m_relax);
    rl->addWidget(m_clear);
    rl->addStretch(1);
    lay->addWidget(row);
  }
  void setMessage(const QString &msg) { m_text->setText(msg); }

signals:
  void relaxRequested(); // 去掉最后一个条件（逐级放宽）
  void clearRequested();

private:
  QLabel *m_text = nullptr;
  QPushButton *m_relax = nullptr;
  QPushButton *m_clear = nullptr;
};

} // namespace paleo::dataops

Q_DECLARE_METATYPE(paleo::dataops::FilterCondition)
Q_DECLARE_METATYPE(paleo::dataops::FilterDim)
