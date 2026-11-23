// 层：功能
#include "chatresultstore.h"
#include "../services/toolresultprotocol.h"
#include <QJsonArray>

namespace paleo::chat {
namespace {
int codepoints(const QString &text) {
  int count = 0;
  for (qsizetype i = 0; i < text.size(); ++i) {
    if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) ++i;
    ++count;
  }
  return count;
}
QString childSelector(const QString &parent, QString key) {
  key.replace("~", "~0");
  key.replace("/", "~1");
  return parent + '/' + key;
}
// H7：页的合格判据是「最终可发送 envelope」而不是内层 JSON 片段。计价对象与
// ChatResultStore::encode 的成功分支同一形状（含 transport.maxBytes、coverage
// 与 nextCursor 的可变字节），因此成功页不会再在最终包装时被降级。
bool fitsEnvelope(const QJsonObject &page, int envelopeBytes) {
  return resultEnvelopeBytes(page, true, envelopeBytes) <= envelopeBytes;
}
} // namespace

QJsonObject ChatResultStore::stringPage(const QString &reference, const QString &selector,
                                       const QString &value, Position position, int envelopeBytes) {
  if (position.offset > value.size()) return {{"code", "invalid_result_cursor"}};
  const int total = codepoints(value);
  const int offset = codepoints(value.left(position.offset));
  auto page = [&](int end) {
    const auto fragment = value.mid(position.offset, end - position.offset);
    QJsonObject out{{"code", "ok"}, {"result_ref", reference}, {"selector", selector},
        {"mode", "text_parts"}, {"kind", "string"}, {"valueFragment", true},
        {"offsetCodepoints", offset}, {"totalCodepoints", total},
        {"returnedCodepoints", codepoints(fragment)}, {"text", fragment},
        {"hasMore", end < value.size()}};
    if (end < value.size()) out.insert("nextCursor", cursor(reference, selector, position.mode, end));
    return out;
  };
  const auto complete = page(int(value.size()));
  if (fitsEnvelope(complete, envelopeBytes)) return complete;
  int low = position.offset, high = int(value.size());
  while (low < high) {
    const int middle = low + (high - low + 1) / 2;
    if (fitsEnvelope(page(middle), envelopeBytes)) low = middle;
    else high = middle - 1;
  }
  if (low > position.offset && low < value.size() && value[low - 1].isHighSurrogate() && value[low].isLowSurrogate())
    --low;
  auto out = page(low);
  if ((low == position.offset && low < value.size()) || !fitsEnvelope(out, envelopeBytes))
    return {{"code", "value_exceeds_page_budget"}};
  return out;
}
QJsonObject ChatResultStore::containerPage(const QString &reference, const QString &selector,
    const QJsonValue &value, Position position, int pageSize, int envelopeBytes) {
  const bool arrayKind = value.isArray();
  const auto array = value.toArray();
  const auto object = value.toObject();
  const auto keys = object.keys();
  const int total = int(arrayKind ? array.size() : keys.size());
  if (position.offset > total) return {{"code", "invalid_result_cursor"}};
  auto page = [&](const QJsonArray &entries, int end) {
    bool hasChild = false;
    for (const auto &entry : entries) hasChild |= entry.toObject().contains("child");
    QJsonObject out{{"code", "ok"}, {"result_ref", reference}, {"selector", selector},
        {"kind", position.mode}, {"offset", position.offset}, {"entries", entries},
        {"returnedCount", entries.size()}, {"totalCount", total}, {"hasMore", end < total},
        {"coverage", QJsonObject{{"complete", end == total && !hasChild},
          {"scope", "page"}, {"requiresChildExpansion", hasChild}}}};
    if (end < total) out.insert("nextCursor", cursor(reference, selector, position.mode, end));
    return out;
  };
  QJsonArray entries;
  int end = position.offset;
  while (end < total && entries.size() < pageSize) {
    const auto key = arrayKind ? QString::number(end) : keys[end];
    const QJsonValue item = arrayKind ? array[end] : object.value(key);
    QJsonObject entry{{arrayKind ? "index" : "key", arrayKind ? QJsonValue(end) : QJsonValue(key)},
                       {"value", item}};
    auto trial = entries;
    trial.append(entry);
    if (!fitsEnvelope(page(trial, end + 1), envelopeBytes)) {
      // 若该成员单独一页能放下，只是本页剩余预算不够：把它留给下一页
      // （页首重新获得完整预算），避免把后续每个正常成员都降级成 child。
      const bool fitsAlone = fitsEnvelope(page(QJsonArray{entry}, end + 1), envelopeBytes);
      if (!entries.isEmpty() && fitsAlone) break;
      // 本身超过一页的成员：在同一不可变值内描述其位置（不新建引用）。
      const auto child = childSelector(selector, key);
      entry.remove("value");
      entry.insert("child", QJsonObject{{"selector", child}, {"kind", jsonValueKind(item)}});
      trial = entries;
      trial.append(entry);
      if (child.toUtf8().size() > 1024 || !fitsEnvelope(page(trial, end + 1), envelopeBytes)) {
        if (entries.isEmpty()) return {{"code", "value_exceeds_page_budget"}};
        break;
      }
    }
    entries = trial;
    ++end;
  }
  auto out = page(entries, end);
  if (!fitsEnvelope(out, envelopeBytes)) return {{"code", "value_exceeds_page_budget"}};
  return out;
}
} // namespace paleo::chat
