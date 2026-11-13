// 层：数据
#pragma once
#include "seismicattributefusionclient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QUrl>
#include <algorithm>

namespace paleo::fusion_detail {
using Progress = std::function<void(int, const QString &)>;
using Cancel = std::function<bool()>;
inline SeismicFusionResult failure(const QString &error) {
  SeismicFusionResult result;
  result.error = error;
  return result;
}
struct Control {
  QElapsedTimer timer;
  int timeoutMs;
  Cancel cancel;
  Control(int timeout, Cancel callback) : timeoutMs(timeout), cancel(std::move(callback)) { timer.start(); }
  int remaining() const { return int(std::max(qint64(0), qint64(timeoutMs) - timer.elapsed())); }
  QString interruption() const {
    if (cancel && cancel()) return SeismicAttributeFusionClient::tr("Attribute fusion cancelled.");
    if (!remaining()) return SeismicAttributeFusionClient::tr("Attribute fusion timed out.");
    return {};
  }
};
QString validate(const SeismicFusionRequest &request);
bool localUrl(const QUrl &url);
bool health(const QString &url, Control &control);
SeismicFusionResult http(const SeismicFusionRequest &, const Progress &, Control &);
SeismicFusionResult cli(const SeismicFusionRequest &, const Progress &, Control &);
SeismicFusionResult parse(const QJsonObject &, const QString &, qint64, Control * = nullptr);
} // namespace paleo::fusion_detail
