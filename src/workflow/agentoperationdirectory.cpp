// 层：功能
#include "agentoperationdirectory.h"

#include "../ai/chat/domaintools.h"
#include "../services/capabilitydescription.h"
#include "../services/commandspec.h"

#include <QCoreApplication>
#include <algorithm>

namespace paleo::agent {
QVector<DirectoryEntry> buildDirectory(const QString &filter, const OperationSearch &search) {
  QVector<DirectoryEntry> entries;
  const auto add = [&](int kind, const QString &id, int version, const QJsonObject &card,
                       QStringList names, const QStringList &details) {
    names.append(id);
    const double score = search.score(names, details);
    if (score > 0)
      entries.append({kind, id, version, card, score});
  };
  if (filter == QLatin1String("default") || filter == QLatin1String("capabilities"))
    for (const auto &spec : CapabilityRegistry::all()) {
      QStringList names = spec.aliases;
      if (!search.phrase.isEmpty() && !spec.algorithmId.isEmpty())
        names.append(AlgorithmSpecRegistry::specAtVersion(spec.algorithmId, spec.algorithmContractVersion).displayName);
      add(0, spec.capabilityId, spec.contractVersion, capability::capabilitySummaryCard(spec),
          names, {spec.algorithmId});
    }
  if (filter == QLatin1String("default") || filter == QLatin1String("algorithms"))
    for (const auto &spec : AlgorithmSpecRegistry::allSpecsProjection())
      add(1, spec.id, spec.contractVersion, capability::algorithmSummaryCard(spec),
          {spec.displayName}, {spec.engine});
  if (filter == QLatin1String("default") || filter == QLatin1String("tools")) {
    for (const AiToolSpec &spec : builtinAiToolSpecs()) {
      QJsonObject card{{"kind", QStringLiteral("tool")}, {"id", spec.name}, {"title", spec.title},
                       {"requires_local_ort", spec.requiresLocalOrt}, {"legacy_adapter", spec.legacyAdapter},
                       {"exposure", capabilityExposureName(spec.exposure)}};
      add(2, spec.name, 1, card, {spec.title}, {spec.description});
    }
  }
  if (filter == QLatin1String("commands"))
    for (const auto &spec : command::CommandRegistry::all()) {
      QJsonObject card = capability::commandSummaryCard(spec);
      QStringList names, details;
      if (!search.phrase.isEmpty()) {
        const QString title = QCoreApplication::translate("PaleoCommands", spec.title);
        names = command::commandTitleAliases(spec);
        names.append(title);
        details = {QCoreApplication::translate("PaleoCommands", spec.tooltip), spec.ribbonGroup};
        card.insert(QStringLiteral("title"), title);
      }
      add(3, spec.commandId, spec.contractVersion, card, names, details);
    }
  std::sort(entries.begin(), entries.end(), [](const DirectoryEntry &a, const DirectoryEntry &b) {
    if (a.score != b.score)
      return a.score > b.score;
    if (a.kindRank != b.kindRank)
      return a.kindRank < b.kindRank;
    if (a.id != b.id)
      return a.id < b.id;
    return a.version < b.version;
  });
  return entries;
}
} // namespace paleo::agent
