// 层：数据
#include "seismicattributefusionclient_internal.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <memory>

namespace paleo::fusion_detail {
namespace {
constexpr qint64 maxJsonBytes = 8 * 1024 * 1024;
struct Response {
  QJsonObject object;
  QString error;
};

Response exchange(QNetworkAccessManager &network, const QUrl &url, const QByteArray *body,
                  Control &control, int phaseTimeoutMs)
{
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  std::unique_ptr<QNetworkReply> reply(body ? network.post(request, *body) : network.get(request));
  reply->setReadBufferSize(maxJsonBytes + 1);
  QEventLoop loop;
  QTimer tick;
  QElapsedTimer elapsed;
  elapsed.start();
  QByteArray bytes;
  QString error;
  const auto check = [&] {
    if (reply->isOpen()) bytes += reply->read(maxJsonBytes + 1 - bytes.size());
    error = control.interruption();
    if (error.isEmpty() && bytes.size() > maxJsonBytes) error = SeismicAttributeFusionClient::tr("Service JSON exceeds the size limit.");
    if (error.isEmpty() && elapsed.elapsed() >= phaseTimeoutMs) error = SeismicAttributeFusionClient::tr("Local service request timed out.");
    if (!error.isEmpty()) { if (reply->isRunning()) reply->abort(); loop.quit(); }
  };
  QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, check);
  QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&tick, &QTimer::timeout, &loop, check);
  tick.start(20);
  if (!reply->isFinished()) loop.exec();
  check();
  if (!error.isEmpty()) return {{}, error};
  QJsonParseError jsonError;
  const auto document = QJsonDocument::fromJson(bytes, &jsonError);
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
    auto detail = document.object().value(QStringLiteral("detail")).toString();
    if (detail.isEmpty()) detail = document.object().value(QStringLiteral("error")).toString();
    if (detail.isEmpty()) detail = reply->errorString();
    return {{}, SeismicAttributeFusionClient::tr("Local service HTTP %1: %2").arg(status).arg(detail)};
  }
  if (jsonError.error != QJsonParseError::NoError || !document.isObject())
    return {{}, SeismicAttributeFusionClient::tr("Local service returned invalid JSON.")};
  return {document.object(), {}};
}

void pause(Control &control)
{
  QEventLoop loop;
  QTimer tick;
  QElapsedTimer elapsed;
  elapsed.start();
  QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
    if (elapsed.elapsed() >= 200 || !control.interruption().isEmpty()) loop.quit();
  });
  tick.start(20);
  loop.exec();
}

void cancelJob(QNetworkAccessManager &network, const QUrl &url)
{
  // Cleanup has its own bounded grace period after the caller's work deadline.
  Control cleanup(1000, {});
  const QByteArray body("{}");
  exchange(network, url, &body, cleanup, 1000);
}
} // namespace

bool health(const QString &url, Control &control)
{
  QNetworkAccessManager network;
  network.setProxy(QNetworkProxy::NoProxy);
  const auto response = exchange(network, QUrl(url).resolved(QUrl(QStringLiteral("/health"))),
                                 nullptr, control, std::min(1200, control.remaining()));
  const auto &object = response.object;
  return response.error.isEmpty() && object.value(QStringLiteral("status")) == QLatin1String("ok") &&
      object.value(QStringLiteral("service")) == QLatin1String("seismic-attribute-fusion") &&
      object.value(QStringLiteral("api_version")) == QLatin1String("1.0");
}

SeismicFusionResult http(const SeismicFusionRequest &request, const Progress &progress, Control &control)
{
  QNetworkAccessManager network;
  network.setProxy(QNetworkProxy::NoProxy);
  const QUrl base(request.serverUrl);
  auto body = SeismicAttributeFusionClient::buildRequestJson(request);
  body.insert(QStringLiteral("wait"), false);
  const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
  const auto submitted = exchange(network, base.resolved(QUrl(QStringLiteral("/api/v1/jobs"))),
                                  &bytes, control, std::min(10000, control.remaining()));
  if (!submitted.error.isEmpty()) return failure(submitted.error);
  const auto jobId = submitted.object.value(QStringLiteral("job_id")).toString();
  // IDs are one path segment. Never interpolate a server-supplied URL or traversal.
  if (jobId.isEmpty() || jobId.size() > 128 ||
      std::any_of(jobId.begin(), jobId.end(), [](QChar c) {
        return !((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                 (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                 c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('_'));
      })) return failure(SeismicAttributeFusionClient::tr("Service did not return a valid job ID."));
  const auto statusUrl = base.resolved(QUrl(QStringLiteral("/api/v1/jobs/%1").arg(jobId)));
  const auto cancelUrl = base.resolved(QUrl(QStringLiteral("/api/v1/jobs/%1/cancel").arg(jobId)));
  const auto abort = [&](const QString &error) {
    cancelJob(network, cancelUrl);
    return failure(error);
  };
  int lastPercent = -1;
  QString lastDetail;
  while (true) {
    const auto stopped = control.interruption();
    if (!stopped.isEmpty()) return abort(stopped);
    const auto response = exchange(network, statusUrl, nullptr, control, std::min(5000, control.remaining()));
    if (!response.error.isEmpty()) return abort(response.error);
    const auto &object = response.object;
    const auto state = object.value(QStringLiteral("state")).toString();
    const auto responseId = object.value(QStringLiteral("job_id")).toString();
    if (!responseId.isEmpty() && responseId != jobId) return abort(SeismicAttributeFusionClient::tr("Service returned a different job ID."));
    const int percent = object.value(QStringLiteral("percent")).toInt(-1);
    const auto detail = object.value(QStringLiteral("detail")).toString();
    if (percent < 0 || percent > 100) return abort(SeismicAttributeFusionClient::tr("Service returned invalid progress."));
    if (progress && (percent != lastPercent || detail != lastDetail)) progress(percent, detail);
    lastPercent = percent;
    lastDetail = detail;
    if (state == QLatin1String("succeeded"))
      return parse(object, request.outputDir, request.maxResultCells, &control);
    if (state == QLatin1String("failed") || state == QLatin1String("cancelled")) {
      auto error = object.value(QStringLiteral("error")).toString();
      if (error.isEmpty()) error = SeismicAttributeFusionClient::tr("Attribute fusion service failed or was cancelled.");
      return failure(error);
    }
    if (state != QLatin1String("queued") && state != QLatin1String("running"))
      return abort(SeismicAttributeFusionClient::tr("Service returned an unknown job state."));
    pause(control);
  }
}
} // namespace paleo::fusion_detail
