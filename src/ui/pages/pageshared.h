// 层：视图
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include "../paleotheme.h"

// ui/pages/pageshared — 各页面板共用的视图助手（W5b：自 pagepanels.cpp
// 的匿名命名空间收敛成内联头）。只含纯 Qt 布局/查找助手，不含业务。
namespace paleo::pagesinternal
{

// 服务绑定走动态 QObject 属性（pagepanels.h 的公共形态不声明数据成员；
// 与 workflows.cpp 同一惯例），子控件按 objectName 查找。
inline constexpr char kLayersProp[] = "paleo.page.layers"; // QObject* (QgisLayerService)
inline constexpr char kWfProp[] = "paleo.page.wf";         // QObject* (page workflow)
inline constexpr char kTaskSvcProp[] = "paleo.page.tasksvc"; // QObject* (PaleoTaskService)

// PW-043：服务属性的生命周期跟踪绑定——被绑对象销毁时自动清空属性，读侧
// qobject_cast 拿到 nullptr 而不是悬挂指针（QVariant 里的 QObject* 不是
// QPointer）。重复绑定时只清空仍指向已毁对象的那一个。
inline void bindTrackedProperty(QObject *owner, const char *name, QObject *target)
{
  owner->setProperty(name, target ? QVariant::fromValue(target) : QVariant());
  if (!target)
    return;
  QObject::connect(target, &QObject::destroyed, owner, [owner, name, target] {
    if (owner->property(name).value<QObject *>() == target)
      owner->setProperty(name, QVariant());
  });
}

// 六页序表：ribbon 页签序 = 阅读序 = 右栏栈序。壳（paleomainwindow.cpp）
// 与接线 TU（paleomainwindow_attach.cpp）共用，W4 后两处 TU 都要查序。
inline const QStringList kPageIds = {
  QStringLiteral("data"),       // 数据管理
  QStringLiteral("correlation"), // 地层对比（独立 Web 工作台）
  QStringLiteral("predict"),    // 智能预测
  QStringLiteral("constraint"), // 单因素图
  QStringLiteral("compose"),    // 智能编图
  QStringLiteral("validate"),   // 验证
};

// DESIGN.md `label` token: 8pt muted captions mark panel groups.
inline QLabel *caption(const QString &text, QWidget *parent)
{
  auto *l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(PaleoTheme::tokens().labelPt);
  l->setFont(f);
  l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  // Register live: captions already on screen must follow theme changes.
  PaleoTheme::applyThemedStyleSheet(l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  return l;
}

// 纯 Qt 折叠段控件（DESIGN.md surface-alt 底、浅边框、▼/▶ 开合）。
// chrome 全 token，applyThemedStyleSheet 活体注册随主题重算（原
// panelshared.h 的硬编码浅色版已收敛至此——三编图页与 entitypanel 共用）。
class CollapsibleSection : public QWidget
{
public:
  explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr)
    : QWidget(parent)
    , m_title(title)
  {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(PaleoTheme::tokens().spacingXs);

    // QPushButton：QSS text-align 只对按钮类生效，QToolButton 会把标题居中成
    // 一条「按钮带」。折叠组标题左对齐，与 QgsCollapsibleGroupBox 一致。
    m_toggle = new QPushButton(this);
    m_toggle->setObjectName(QStringLiteral("collapsibleSectionToggle"));
    m_toggle->setFlat(true);
    m_toggle->setText(QStringLiteral("▼  ") + title);
    m_toggle->setCheckable(true);
    m_toggle->setChecked(true);
    PaleoTheme::applyThemedStyleSheet(m_toggle, [] {
      const auto &t = PaleoTheme::tokens();
      return PaleoTheme::metricStyleSheet(QStringLiteral(
                 "QPushButton { "
                 "  font-weight: 600; "
                 "  font-size: {typography.label}pt; "
                 "  color: %1; "
                 "  background: %2; "
                 "  border: 1px solid %3; "
                 "  border-radius: {rounded.sm}px; "
                 "  padding: {spacing.xs}px {spacing.sm}px; "
                 "  text-align: left; "
                 "} "
                 "QPushButton:hover { background: %4; }"))
          .arg(t.text.name().toUpper(), t.surfaceAltRaised.name().toUpper(),
               t.border.name().toUpper(),
               t.surfaceAlt.name().toUpper());
    });
    m_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_container = new QWidget(this);
    auto *cl = new QVBoxLayout(m_container);
    cl->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs);
    cl->setSpacing(PaleoTheme::tokens().spacingXs);

    lay->addWidget(m_toggle);
    lay->addWidget(m_container);

    QObject::connect(m_toggle, &QPushButton::toggled, this, [this](bool checked) {
      m_container->setVisible(checked);
      m_toggle->setText((checked ? QStringLiteral("▼  ") : QStringLiteral("▶  ")) + m_title);
    });
  }

  QWidget *container() const { return m_container; }
  QVBoxLayout *containerLayout() const { return qobject_cast<QVBoxLayout *>(m_container->layout()); }
  void setExpanded(bool exp) { m_toggle->setChecked(exp); }

private:
  QString m_title;
  QPushButton *m_toggle = nullptr;
  QWidget *m_container = nullptr;
};

// 窄停靠栏：下拉框勿按最长选项撑宽整页（弹出列表仍显示全文）。
inline void shrinkComboBoxesToDock(QWidget *root)
{
  for (QComboBox *c : root->findChildren<QComboBox *>()) {
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(6);
  }
}

// 主操作按钮（DESIGN.md：primary #1B73D0 三用途之一 = 主按钮）——primary
// 底 + onPrimary 字 + primaryHover hover，活体注册随主题。每页至多一颗。
inline void markPrimaryButton(QPushButton *btn)
{
  if (!btn)
    return;
  btn->setDefault(true);
  btn->setAutoDefault(true);
  PaleoTheme::applyThemedStyleSheet(btn, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { "
               "  background: %1; color: %2; border: 1px solid %1; "
               "  border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.md}px; font-weight: 600; "
               "} "
               "QPushButton:hover { background: %3; border-color: %3; } "
               "QPushButton:disabled { background: %4; color: %5; border-color: %4; }"))
        .arg(t.primary.name().toUpper(), t.onPrimary.name().toUpper(),
             t.primaryHover.name().toUpper(), t.surfaceAltRaised.name().toUpper(),
             t.textDisabled.name().toUpper()) + PaleoTheme::focusRingStyleSheet();
  });
}

inline QVBoxLayout *panelLayout(QWidget *page)
{
  auto *lay = new QVBoxLayout(page);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // spacing.sm
  lay->setSpacing(PaleoTheme::tokens().spacingSm);
  return lay;
}

template <typename T>
T *child(QObject *root, const char *name)
{
  return root->findChild<T *>(QLatin1String(name));
}

} // namespace paleo::pagesinternal
