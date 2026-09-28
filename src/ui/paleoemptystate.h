// 层：视图
#pragma once
#include <QLabel>

class QWidget;

// ui/paleoemptystate — 空态/降级态共享卡片（wave/ux-polish）。
// 原是 paleomainwindow.cpp 匿名命名空间私有（T31），layertreepanel 有复制
// 版——收敛为共享组件：三态规范（空数据/失败/降级）见 docs/progress/ux.md。
// 样式走 PaleoTheme::emptyStateStyleSheet 并随主题切换活体重算；宿主
// resize 时保持居中；文案永远带下一步动作指引（DESIGN.md 空态约定）。
class PaleoEmptyStateLabel : public QLabel
{
  public:
    // kind 进 objectName（emptyStateCard / emptyStateCardError /
    // emptyStateCardDegraded），测试与 a11y 可分辨三态。
    enum class Kind { Empty, Error, Degraded };

    PaleoEmptyStateLabel(const QString &text, QWidget *host,
                         Kind kind = Kind::Empty);

    // 失败/降级态换文案（空态文案常驻，用构造时的那份）。
    void setDetailText(const QString &text);

  protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

  private:
    void recenter(const QSize &host);
    Kind m_kind;
};
