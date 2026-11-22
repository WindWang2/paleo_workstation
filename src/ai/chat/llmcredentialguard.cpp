// 层：功能
#include "llmcredentialguard.h"

#include <QJsonArray>

namespace paleo::chat {
namespace {
// 旧字节包含防线保留的包含式检测门槛：短于它的 key 做包含匹配会把任意
// 正常文本（比如单字符 key 出现在模型名里）误判成泄露；等值匹配对任意
// 长度都成立，覆盖短 key 的回显场景。
constexpr qsizetype kContainsMatchMinBytes = 8;
} // namespace

bool valueDisclosesCredential(const QString &value, const QByteArray &apiKey) {
  if (apiKey.isEmpty())
    return false;
  const QString key = QString::fromUtf8(apiKey);
  if (value == key)
    return true;
  if (apiKey.size() >= kContainsMatchMinBytes && value.contains(key))
    return true;
  return false;
}

bool jsonDisclosesCredential(const QJsonValue &value, const QByteArray &apiKey) {
  if (apiKey.isEmpty())
    return false;
  if (value.isString())
    return valueDisclosesCredential(value.toString(), apiKey);
  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array)
      if (jsonDisclosesCredential(item, apiKey))
        return true;
    return false;
  }
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
      // 对象键同样是模型目录的字符串面：键名等值回显也按泄露处理。
      if (valueDisclosesCredential(it.key(), apiKey))
        return true;
      if (jsonDisclosesCredential(it.value(), apiKey))
        return true;
    }
  }
  return false;
}

QString redactCredential(QString text, const QByteArray &apiKey) {
  if (apiKey.isEmpty())
    return text;
  // 与 WellFaciesService::redactKey 同款占位符口径（空 key 先行跳过：
  // QString::replace("") 会在每个字符边界插入占位符）。
  text.replace(QString::fromUtf8(apiKey), QStringLiteral("[已隐藏]"));
  return text;
}

} // namespace paleo::chat
