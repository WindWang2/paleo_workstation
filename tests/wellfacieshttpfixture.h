// 预测专项 P1：测井相远端服务的 mock-HTTP 合同测试（不连任何真实服务）。
// 补 tst_wellfacies 未覆盖的边：模型清单 schema 过滤、202 无任务 ID、轮询换
// 任务、结果模型不一致、未知状态、错误/进度回显密钥脱敏、重定向不跟随、
// 取消后迟到响应被忽略、取消停止轮询、无密钥不发请求。
#include "ai/wellfaciesservice.h"
#include "domain/welllogfacies.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
#include <memory>

using namespace WellComposite;
namespace {
constexpr const char *kKey = "mock-key-0123456789";

struct Reply {
  int status = 200;
  QByteArray payload;
  QByteArray extraHeaders;
  bool hold = false; // 不回应，留待测试手动释放
};

// 每个请求按 handler 脚本应答；记录路径与请求头供断言。
class ScriptServer {
public:
  QTcpServer server;
  QList<QByteArray> paths;
  QList<QByteArray> raw;
  std::function<Reply(const QByteArray &path, const QJsonObject &body)> handler;
  QList<QPointer<QTcpSocket>> held;
  QList<Reply> heldReplies;
  ScriptServer() {
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
      while (server.hasPendingConnections()) {
        auto *s = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buffer] {
          *buffer += s->readAll();
          const auto end = buffer->indexOf("\r\n\r\n");
          if (end < 0) return;
          qsizetype length = 0;
          for (const auto &h : buffer->left(end).split('\n'))
            if (h.toLower().startsWith("content-length:"))
              length = h.mid(15).trimmed().toLongLong();
          if (buffer->size() < end + 4 + length) return;
          const QByteArray request = *buffer;
          buffer->clear();
          raw << request;
          const auto path = request.split(' ').value(1);
          paths << path;
          const auto body = QJsonDocument::fromJson(request.mid(end + 4)).object();
          const Reply reply = handler ? handler(path, body) : Reply{404, "{}"};
          if (reply.hold) {
            held << s;
            heldReplies << reply;
            return;
          }
          send(s, reply);
        });
      }
    });
  }
  static void send(QTcpSocket *s, const Reply &r) {
    s->write("HTTP/1.1 " + QByteArray::number(r.status) +
             " X\r\nContent-Type: application/json\r\nConnection: close\r\n" + r.extraHeaders +
             "Content-Length: " + QByteArray::number(r.payload.size()) + "\r\n\r\n" + r.payload);
    s->disconnectFromHost();
  }
  void release() {
    for (int i = 0; i < held.size(); ++i)
      if (held[i]) send(held[i], heldReplies[i]);
    held.clear();
    heldReplies.clear();
  }
  int count(const QByteArray &suffix) const {
    int n = 0;
    for (const auto &p : paths) n += p.endsWith(suffix) || (suffix.endsWith('/') && p.contains(suffix));
    return n;
  }
  WellFaciesConfig config(const QByteArray &key = kKey) const {
    WellFaciesConfig c;
    c.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1").arg(server.serverPort()));
    c.apiKey = key;
    return c;
  }
};

}
