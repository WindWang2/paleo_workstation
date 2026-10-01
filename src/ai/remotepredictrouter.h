// 层：功能
#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

#include "remotepredictionservice.h"

class QNetworkAccessManager;
class QNetworkReply;
class PaleoOnnxService;

// ai/ — 远端推理路由（goal/ai-geological-assist 范围6）。
// 策略：先健康检查（/health，超时判死）→ 健康走远端预测（/predict，超时
// 判失败）→ 任一环节失败且允许降级 → 本地 ORT 跑同一网格（请求须携带
// samples 数据——没有数据就不造假，如实失败）。全部环节错误链保留原始
// 信息。无默认外网端点：base URL 由调用方显式给（测试用 loopback）。

// 传输抽象：测试注入假服务器；产品实现是 HTTP（QNetworkAccessManager）。
class RemoteTransport
{
  public:
    virtual ~RemoteTransport();
    // 阻塞式（内部事件循环等待），timeoutMs 后判死。
    virtual bool healthCheck( int timeoutMs, QString *error ) = 0;
    // 执行预测：cells 为 columns×rows 行主序相码（255=nodata）。失败 false。
    virtual bool predict( const RemotePredictionRequest &request, int timeoutMs,
                          QVector<int> &cells, QString *error ) = 0;
};

class HttpRemoteTransport : public QObject, public RemoteTransport
{
    Q_OBJECT
  public:
    explicit HttpRemoteTransport( const QUrl &base, QObject *parent = nullptr );
    ~HttpRemoteTransport() override;
    bool healthCheck( int timeoutMs, QString *error ) override;
    bool predict( const RemotePredictionRequest &request, int timeoutMs,
                  QVector<int> &cells, QString *error ) override;

  private:
    // 同步等待 reply 完成（事件循环 + 定时器）；超时 abort 并返回 nullptr。
    QNetworkReply *waitFinished( class QNetworkReply *reply, int timeoutMs, QString *error );

    QUrl m_base;
    QNetworkAccessManager *m_nam = nullptr;
};

// 路由器：实现既有 RemotePredictionService 接口（mappingworkbench 消费面
// 零改动）。start() 异步（QTimer::singleShot(0) 链），进度按阶段发
// （健康检查/远端推理/本地降级），取消在各阶段间检查。
class RemotePredictionRouter : public RemotePredictionService
{
    Q_OBJECT
  public:
    explicit RemotePredictionRouter( RemoteTransport *transport, QObject *parent = nullptr );
    ~RemotePredictionRouter() override;

    // 本地 ORT 降级（可选）：服务 + 分类模型名（如 "seg3"）。未绑或请求无
    // samples → 远端失败即失败（不造假数据）。
    void setLocalFallback( PaleoOnnxService *onnx, const QString &model );
    void setFallbackEnabled( bool enabled );       // 默认 true（绑了本地才有意义）
    void setHealthTimeoutMs( int ms ) { m_healthTimeoutMs = ms; }
    void setPredictTimeoutMs( int ms ) { m_predictTimeoutMs = ms; }

    void start( const RemotePredictionRequest &request ) override;
    void cancel() override;

    // 诊断：最近一次请求走了哪条路（"remote" / "local-ort:<model>" / 空）。
    QString lastRoute() const { return m_lastRoute; }

  private:
    bool runLocalFallback( const RemotePredictionRequest &request, QVector<int> &cells,
                           QString *error );

    RemoteTransport *m_transport = nullptr;
    PaleoOnnxService *m_onnx = nullptr;
    QString m_localModel;
    bool m_fallbackEnabled = true;
    int m_healthTimeoutMs = 2000;
    int m_predictTimeoutMs = 10000;
    QString m_lastRoute;
    bool m_cancelled = false;
};
