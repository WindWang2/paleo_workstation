// 层：视图
#pragma once
// ui/pages/panelshared.h — 右 dock 页面板共享的小控件与助手。
// 原 pagepanels.cpp 匿名命名空间成员抽出，供拆分后的 per-page 文件
// （pagepanels/predictpage/constraintpage/composepage）共用；行为与样式
// 逐字保持（DESIGN.md token 内嵌于样式表）。

#include <QLabel>
#include <QObject>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

// pagepanels.h fixes the public shape of the page classes and declares no
// data members, so service bindings ride on dynamic QObject properties (same
// idiom as workflows.cpp) and child widgets are located by objectName.
namespace PaleoPanel
{
  inline constexpr const char *kLayersProp  = "paleo.page.layers"; // QObject* (QgisLayerService)
  inline constexpr const char *kWfProp      = "paleo.page.wf";     // QObject* (page workflow)
  inline constexpr const char *kTaskSvcProp = "paleo.page.tasksvc"; // QObject* (PaleoTaskService)

  // 纯 Qt 折叠段控件（DESIGN.md 浅灰 surface-alt，浅边框，▼/▶ 开合）
  class CollapsibleSection : public QWidget
  {
  public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr)
      : QWidget(parent)
      , m_title(title)
    {
      auto *lay = new QVBoxLayout(this);
      lay->setContentsMargins(0, 0, 0, 0);
      lay->setSpacing(4);

      m_toggle = new QToolButton(this);
      m_toggle->setText(QStringLiteral("▼  ") + title);
      m_toggle->setCheckable(true);
      m_toggle->setChecked(true);
      m_toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
      m_toggle->setStyleSheet(QStringLiteral(
          "QToolButton { "
          "  font-weight: 600; "
          "  font-size: 8.5pt; "
          "  color: #24303E; "
          "  background: #EDF1F5; "
          "  border: 1px solid #DFE5EC; "
          "  border-radius: 4px; "
          "  padding: 4px 8px; "
          "  text-align: left; "
          "} "
          "QToolButton:hover { background: #E2E8F0; }"));
      m_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

      m_container = new QWidget(this);
      auto *cl = new QVBoxLayout(m_container);
      cl->setContentsMargins(4, 2, 4, 4);
      cl->setSpacing(4);

      lay->addWidget(m_toggle);
      lay->addWidget(m_container);

      QObject::connect(m_toggle, &QToolButton::toggled, this, [this](bool checked) {
        m_container->setVisible(checked);
        m_toggle->setText((checked ? QStringLiteral("▼  ") : QStringLiteral("▶  ")) + m_title);
      });
    }

    QWidget *container() const { return m_container; }
    QVBoxLayout *containerLayout() const { return static_cast<QVBoxLayout *>(m_container->layout()); }
    void setExpanded(bool exp) { m_toggle->setChecked(exp); }

  private:
    QString m_title;
    QToolButton *m_toggle = nullptr;
    QWidget *m_container = nullptr;
  };

  // DESIGN.md `label` token: 8pt muted captions mark panel groups.
  inline QLabel *caption(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
    return l;
  }

  inline QVBoxLayout *panelLayout(QWidget *page)
  {
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
    lay->setSpacing(8);
    return lay;
  }

  template <typename T>
  T *child(QObject *root, const char *name)
  {
    return root->findChild<T *>(QLatin1String(name));
  }
} // namespace PaleoPanel
