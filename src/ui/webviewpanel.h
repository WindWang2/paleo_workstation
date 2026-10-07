// 层：视图
#pragma once
#include <QUrl>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedLayout;
class QWebEngineView;
class QWebEngineProfile;

// ui/webviewpanel — 内嵌浏览器孤岛：为「嵌壳」用途（已开发 web 服务 / 页面
// 预览与可视化）提供 QWebEngineView 宿主。
//
// 设计约束：
//   · 懒初始化——首个 setUrl() 才创建 view / 拉起 QtWebEngineProcess，app
//     启动不为此付成本。
//   · 降级而非崩溃——无屏平台（offscreen，ctest）、初始化失败或渲染进程
//     终止时切到说明面 +「用系统浏览器打开」，绝不拖死宿主。
//   · main() 必须在 QApplication 之前 setAttribute(Qt::AA_ShareOpenGLContexts)
//    （与 QGIS map canvas 共用 GL context）。
class WebViewPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WebViewPanel(QWidget *parent = nullptr);
    ~WebViewPanel() override;

    // 开始加载 url。返回 true 表示引擎已建并开始加载（结果经 loadFinished
    // 信号）；false = 引擎不可用，已进入降级面，原因见 lastError()。
    bool setUrl(const QUrl &url);
    // #82：内嵌与「用系统浏览器打开」共用的 scheme 白名单——只放行带主机名的
    // http/https。file:// / smb:// / ftp:// / 自定义协议一律拒绝（不交给
    // QWebEngineView，也不交给 QDesktopServices::openUrl 的系统协议处理器）。
    static bool isAllowedUrl(const QUrl &url);
    // #237：页内导航白名单（acceptNavigationRequest 同款口径）——setUrl 只挡
    // 地址栏式加载，已加载页的链接/表单/302 走的是导航请求，不过同一张表
    // 就能被带到 file:// 或任意外域。about: 放行（JS iframe/window.open 的
    // 常规目标）。offscreen 下引擎不可建，判定必须是可独立回归的纯函数。
    static bool isNavigationAllowed(const QUrl &url);
    QUrl url() const { return m_url; }
    bool engineAvailable() const { return m_engine != nullptr; }
    QString lastError() const { return m_lastError; }
    void showError(const QUrl &url, const QString &reason);
    void setPageStyleSheet(const QString &css);

  signals:
    void loadFinished(bool ok);           // 透传 QWebEngineView::loadFinished
    void loadFailed(const QString &error); // 引擎不可用 / 渲染进程终止

  private:
    bool ensureEngine(QString *error);
    void showFallback(const QString &reason);
    void installPageStyleSheet();

    QWebEngineView *m_engine = nullptr; // 懒建；nullptr = 未建或不可用
    QWebEngineProfile *m_profile = nullptr;
    QStackedLayout *m_stack = nullptr;  // 状态面/引擎页切换（根布局是外壳 VBox）
    QProgressBar *m_progress = nullptr; // 加载进度（>1s 的静默加载必须有反馈）
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_externalButton = nullptr;
    QString m_lastError;
    QUrl m_url;
    QString m_pageCss;
};
