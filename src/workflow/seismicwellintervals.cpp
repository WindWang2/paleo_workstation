// 层：功能
#include "seismicwellintervals.h"
#include "algorithms/faciesmapping/formationdominance.h"
#include "domain/faciestaxonomy.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include <cmath>

namespace {
int targetCode(const QVariantMap &leaf, const QVariantList &schema) {
  for (const auto &item : schema) {
    const auto other=item.toMap();
    if (leaf.value("facies")==other.value("facies") && leaf.value("subfacies")==other.value("subfacies") &&
        leaf.value("microfacies")==other.value("microfacies")) return other.value("code").toInt();
  }
  return 0;
}

}

bool readSeismicWellInterval(QVariantMap &source, const QVariantList &schema, const std::function<bool()> &cancel, QString *error) {
  QFile file(source.value("path").toString());
  if (!file.open(QIODevice::ReadOnly)) { *error=QObject::tr("无法读取单井分段预测"); return false; }
  const auto bytes=file.readAll();
  if (QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()!=source.value("sha256").toByteArray()) {
    *error=QObject::tr("单井分段预测文件已改变"); return false;
  }
  const auto doc=QJsonDocument::fromJson(bytes).object();
  if (!doc.value("intervals").isArray() || !doc.value("facies").isArray()) {
    *error=QObject::tr("单井分段预测格式无效"); return false;
  }
  QVariantList frozen;
  if (!FaciesTaxonomy::normalize(doc.value("facies").toArray().toVariantList(), &frozen,error)) return false;
  QVector<FaciesSpan> spans;
  const double topMd=source.value("top").toDouble(), baseMd=source.value("base").toDouble();
  for (const auto &value : doc.value("intervals").toArray()) {
    if (cancel && cancel()) { *error=QObject::tr("已取消"); return false; }
    const auto row=value.toObject(); const double top=row.value("top").toDouble(NAN), base=row.value("base").toDouble(NAN);
    if (!std::isfinite(top) || !std::isfinite(base) || top>=base) { *error=QObject::tr("预测井段顶底无效"); return false; }
    if (base<=topMd || top>=baseMd) continue;
    const auto label=row.value("facies").toString();
    const auto leaf=FaciesTaxonomy::resolve(frozen,label,error);
    if (leaf.isEmpty()) return false;
    const int code=targetCode(leaf,schema);
    if(!code) { *error=QObject::tr("预测微相与所选分类的完整路径不一致"); return false; }
    spans.append({top,base,QString::number(code),code});
  }
  WellComposite::FormationInterval window;
  window.name=QStringLiteral("目标层段"); window.topDepth=topMd; window.bottomDepth=baseMd;
  const auto result=paleo::faciesmapping::dominantFaciesByFormation({window},spans);
  if(result.isEmpty() || result.first().tie) source.insert("exclude_reason", result.isEmpty()?"no_coverage":"thickness_tie");
  else source.insert("single_code",result.first().dominantCode);
  return true;
}
