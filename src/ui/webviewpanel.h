// 层：视图
#pragma once
#include <QUrl>
#include <QWidget>

class QLabel;
class QPushButton;
class QWebEngineView;

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

    // 开始加载 url。返回 true 表示引擎已建并开始加载（结果经 loadFinished
    // 信号）；false = 引擎不可用，已进入降级面，原因见 lastError()。
    bool setUrl(const QUrl &url);
    QUrl url() const { return m_url; }
    bool engineAvailable() const { return m_engine != nullptr; }
    QString lastError() const { return m_lastError; }

  signals:
    void loadFinished(bool ok);           // 透传 QWebEngineView::loadFinished
    void loadFailed(const QString &error); // 引擎不可用 / 渲染进程终止

  private:
    bool ensureEngine(QString *error);
    void showFallback(const QString &reason);

    QWebEngineView *m_engine = nullptr; // 懒建；nullptr = 未建或不可用
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_externalButton = nullptr;
    QString m_lastError;
    QUrl m_url;
};
