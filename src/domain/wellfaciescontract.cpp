// 层：数据
#include "welllogfacies.h"
#include "faciestaxonomy.h"
#include <QObject>
#include <QSet>

bool bindWellFaciesClasses(const WellFaciesModel &model, const QVariantList &schema,
                           WellFaciesInput *input, QString *error)
{
  const auto fail = [error](const QString &reason) {
    if (error) *error = reason;
    return false;
  };
  if (!input) return fail(QObject::tr("缺少测井预测输入"));
  if (model.labels.isEmpty()) return fail(QObject::tr("模型 %1 未声明可识别的微相类别").arg(model.name));
  QStringList labels;
  for (const auto &raw : model.labels) {
    const auto label = raw.trimmed();
    if (label.isEmpty()) return fail(QObject::tr("模型 %1 含空微相类别").arg(model.name));
    if (!labels.contains(label)) labels << label;
  }
  QVariantList frozen;
  QString reason;
  if (!FaciesTaxonomy::extend(schema, labels, &frozen, &reason))
    return fail(QObject::tr("模型 %1 的微相分类不完整：%2").arg(model.name, reason));
  input->facies = frozen;
  input->modelLabels = labels;
  return true;
}

bool resolveWellFaciesClasses(const WellFaciesInput &input, const QStringList &labels,
                              QVariantList *schema, QString *error)
{
  // 旧文件/直达解析调用未绑定模型时，保留既有模板扩展语义。
  if (input.modelLabels.isEmpty()) return FaciesTaxonomy::extend(input.facies, labels, schema, error);
  QVariantList frozen;
  if (!FaciesTaxonomy::normalize(input.facies, &frozen, error)) return false;
  QSet<int> allowed;
  for (const auto &label : input.modelLabels) {
    const auto leaf = FaciesTaxonomy::resolve(frozen, label, error);
    if (leaf.isEmpty()) return false;
    allowed.insert(leaf.value("code").toInt());
  }
  for (const auto &label : labels) {
    const auto leaf = FaciesTaxonomy::resolve(frozen, label, error);
    if (leaf.isEmpty()) return false;
    if (!allowed.contains(leaf.value("code").toInt())) {
      if (error) *error = QObject::tr("预测返回微相「%1」不在模型声明的类别中，未接收该结果").arg(label);
      return false;
    }
  }
  *schema = frozen;
  return true;
}
