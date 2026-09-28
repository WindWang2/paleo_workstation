// 层：功能
#include "remotepredictionservice.h"
#include <QCryptographicHash>
#include <cmath>

MockRemotePredictionService::MockRemotePredictionService(QObject *parent)
    : RemotePredictionService(parent) {
  m_timer.setInterval(25);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    ++m_step;
    emit progress(m_result.request.id, m_step * 10);
    if (m_step < 10)
      return;
    m_timer.stop();
    const auto request = m_result.request;
    const auto digest = QCryptographicHash::hash(
        (request.horizon + request.sourceVersionIds.join('|')).toUtf8(),
        QCryptographicHash::Sha256);
    const int seed = static_cast<unsigned char>(digest[0]);
    const auto code = [&request](int n) {
      return request.facies.at(n % request.facies.size())
          .toMap()
          .value("code")
          .toInt();
    };
    if (request.kind == QLatin1String("seismic")) {
      for (int y = 0; y < request.rows; ++y)
        for (int x = 0; x < request.columns; ++x)
          m_result.cells.append(
              code((x / 12 + y / 18 + seed) % request.facies.size()));
    } else {
      for (const auto &value : request.wells) {
        auto point = value.toMap();
        const auto hash = QCryptographicHash::hash(
            (request.horizon + point.value("id").toString()).toUtf8(),
            QCryptographicHash::Sha256);
        point.insert("facies_code", code(static_cast<unsigned char>(hash[0])));
        point.insert("horizon", request.horizon);
        point.insert("mock", true);
        m_result.points.append(point);
      }
    }
    emit completed(m_result);
  });
}
void MockRemotePredictionService::start(
    const RemotePredictionRequest &request) {
  cancel();
  if (request.facies.isEmpty() || request.columns < 1 || request.rows < 1 ||
      request.columns > 512 || request.rows > 512 ||
      (request.kind != QLatin1String("seismic") &&
       request.kind != QLatin1String("wells"))) {
    emit failed(request.id, tr("预测请求的类型、相分类或网格尺寸无效"));
    return;
  }
  m_result = {};
  m_result.request = request;
  m_result.method = QStringLiteral("mock-remote-facies-v1");
  m_step = 0;
  m_timer.start();
}
void MockRemotePredictionService::cancel() {
  m_timer.stop();
  m_result = {};
}
