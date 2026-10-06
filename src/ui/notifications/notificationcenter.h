// 层：视图
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "../../services/errorhub.h"

class QStatusBar;
class QWidget;
class QEvent;
class ToastCard;

// 方向64：错误呈现层（DESIGN.md「错误呈现」节为视觉契约）。
// 订阅 ErrorHub::errorRaised，按级别分流：
//   info            → 状态栏 showMessage（infoMs）
//   warning / error → 宿主窗口右下角非模态通知卡（warningMs / errorMs 后自动收起）
//   error + severe  → 窗口模态 QMessageBox（open()，不嵌套事件循环），
//                     仅 firstInWindow 时弹——即同去重键 60s 单弹。
// 通知卡预建池（kMaxCards 张）循环复用：通知路径不再 new 控件；第
// kMaxCards+1 张到来时提前收起最旧一张。同键卡在场 → 只刷新「×N」并重置计时。
class NotificationCenter : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxCards = 4;
    static constexpr int kInfoMs = 5000;
    static constexpr int kWarningMs = 8000;
    static constexpr int kErrorMs = 12000;
    static constexpr int kCardWidth = 360;

    NotificationCenter(QWidget *host, ErrorHub *hub);
    ~NotificationCenter() override;

    void setStatusBar(QStatusBar *bar);
    // 测试注入：缩短消失时长（毫秒）。
    void setDismissMsForTest(int infoMs, int warningMs, int errorMs);

    // hub 是否有存活的呈现层（PaleoNotify 据此决定走 hub 还是回落旧模态）。
    static bool hasPresenter(const ErrorHub *hub);
    // hub 当前呈现层的宿主窗口（无则 nullptr）。
    static QWidget *presenterHost(const ErrorHub *hub);

    // ---- 观测面（测试/诊断） ----
    int visibleCardCount() const;
    QStringList visibleCardTexts() const;   // 旧 → 新
    int cardAllocations() const { return m_allocations; }
    int modalShownCount() const { return m_modalShown; }
    int statusShownCount() const { return m_statusShown; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onRaised(const ErrorHub::Entry &entry, bool firstInWindow);
    ToastCard *cardForKey(const QString &key) const;
    ToastCard *takeCard();
    void relayout();
    int dismissMs(ErrorHub::Level level) const;

    QPointer<QWidget> m_host;
    QPointer<ErrorHub> m_hub;
    QPointer<QStatusBar> m_status;
    QVector<ToastCard *> m_cards;   // 预建池，大小恒为 kMaxCards
    quint64 m_seq = 0;
    int m_allocations = 0;
    int m_modalShown = 0;
    int m_statusShown = 0;
    int m_infoMs = kInfoMs;
    int m_warningMs = kWarningMs;
    int m_errorMs = kErrorMs;
};
