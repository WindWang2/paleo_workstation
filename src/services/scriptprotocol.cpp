// 层：数据
#include "scriptprotocol.h"

#include <QJsonDocument>
#include <QJsonObject>

ScriptMessage parseScriptLine(const QString &line)
{
  ScriptMessage msg;
  msg.raw = line;
  const QString trimmed = line.trimmed();
  if (!trimmed.startsWith(QLatin1Char('{')))
    return msg;
  const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
  if (!doc.isObject())
    return msg;
  const QJsonObject obj = doc.object();
  if (obj.value(QStringLiteral("paleo")).toString() != QLatin1String("1"))
    return msg;
  const QString type = obj.value(QStringLiteral("type")).toString();
  if (type == QLatin1String("progress"))
  {
    msg.type = ScriptMessage::Type::Progress;
    msg.percent = obj.value(QStringLiteral("percent")).toInt(-1);
    msg.message = obj.value(QStringLiteral("message")).toString();
  }
  else if (type == QLatin1String("result"))
  {
    msg.type = ScriptMessage::Type::Result;
    msg.path = obj.value(QStringLiteral("path")).toString();
    msg.kind = obj.value(QStringLiteral("kind")).toString();
    msg.message = obj.value(QStringLiteral("message")).toString();
  }
  else if (type == QLatin1String("error"))
  {
    msg.type = ScriptMessage::Type::Error;
    msg.message = obj.value(QStringLiteral("message")).toString();
    msg.code = obj.value(QStringLiteral("code")).toInt(0);
  }
  // type 缺失/未知：保持 Text——不猜测协议意图，原样呈现。
  return msg;
}
