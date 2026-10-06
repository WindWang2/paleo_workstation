#pragma once
// 测试支撑（非产品面）：远端相预测的假服务意识的行为镜像。
//
// 由来：goal/ai-assist-20261006（方向51）把 MockRemotePredictionService 从
// 生产头 src/ai/remotepredictionservice.h 里拆出来——产品装配不再自装 Mock
// （mappingworkbench 构造函数那一行），远端未配置时如实报「未配置」。
// 夹具/用例仍需这份行为（确定性伪随机 phash 分布），故整体迁到 tests/。
//
// 刻意不写 Q_OBJECT：不为双跑 moc——基类 RemotePredictionService 已提供全部
// 信号，派生类不新增信号/槽，缺 metaobject 不影响 emit 与外界 connect。
// 副作用：不可对该类型做 qobject_cast（判定「装配出来的是不是 Mock」走
// metaObject()->className() 字符串断言）。
#include "../src/ai/remotepredictionservice.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QTimer>

class MockRemotePredictionService : public RemotePredictionService {
public:
  explicit MockRemotePredictionService(QObject *parent = nullptr)
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
          QVariantList intervals;
          const double top = point.value("depth_top", 0.0).toDouble();
          const double bottom = point.value("depth_bottom", 120.0).toDouble();
          const int primary = point.value("facies_code").toInt();
          for (int i = 0; i < 12; ++i)
            intervals << QVariantMap{
                {"top", top + (bottom - top) * i / 12.0},
                {"bottom", top + (bottom - top) * (i + 1) / 12.0},
                {"code", i < 8 ? primary
                               : code(static_cast<unsigned char>(hash[0]) + i)}};
          const auto json =
              QString::fromUtf8(QJsonDocument::fromVariant(intervals).toJson(
                  QJsonDocument::Compact));
          point.insert("facies_intervals", json);
          point.insert("predicted_intervals", json);
          point.insert("horizon", request.horizon);
          point.insert("mock", true);
          m_result.points.append(point);
        }
      }
      emit completed(m_result);
    });
  }
  void start(const RemotePredictionRequest &request) override {
    cancel();
    if (request.facies.isEmpty() || request.columns < 1 || request.rows < 1 ||
        request.columns > 512 || request.rows > 512 ||
        (request.kind != QLatin1String("seismic") &&
         request.kind != QLatin1String("wells"))) {
      emit failed(request.id,
                  QObject::tr("预测请求的类型、相分类或网格尺寸无效"));
      return;
    }
    m_result = {};
    m_result.request = request;
    m_result.method = QStringLiteral("mock-remote-facies-v1");
    m_step = 0;
    m_timer.start();
  }
  void cancel() override {
    m_timer.stop();
    m_result = {};
  }

private:
  QTimer m_timer;
  RemotePredictionResult m_result;
  int m_step = 0;
};
