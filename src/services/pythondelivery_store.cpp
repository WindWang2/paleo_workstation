// 层：数据
#include "pythondelivery_internal.h"
#include "pythondelivery_files.h"
#include <QDir>
#include <QDirIterator>

namespace paleo::python::delivery {
namespace {
bool temporary(const QString &name, const QString &prefix) {
  if (!name.startsWith(prefix) || name.size() != prefix.size() + 32) return false;
  for (const auto c : name.mid(prefix.size()))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
bool generations(const Header &header, const QString &root) {
  if (!directory(root, false)) return false;
  QDirIterator entries(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  int count = 0;
  while (entries.hasNext()) {
    const auto path = entries.next(); const auto name = entries.fileName();
    const auto stage = temporary(name, header.kind == "runtime" ? ".stage-" : ".scripts-stage-");
    if (++count > 16384 || !(digest(name) || stage || (header.kind == "runtime" && name == ".leases")) ||
        !directory(path, false)) return false;
  }
  return true;
}
bool selectionName(const Header &header, const QString &name) {
  if (header.kind == "scripts") return name == "current.json" || temporary(name, ".scripts-current-");
  return name == "current-windows-x86_64.json" || name == "current-linux-x86_64.json" || temporary(name, ".current-");
}
} // namespace
bool storeEntries(const Header &header, const QString &store) {
  QDirIterator entries(store, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  int count = 0;
  while (entries.hasNext()) {
    const auto path = entries.next(); const auto name = entries.fileName();
    if (++count > 16384) return false;
    if (name == "generations") { if (!generations(header, path)) return false; continue; }
    const auto lock = header.kind == "runtime" ? ".prepare.lock" : ".scripts.lock";
    if (name == lock || selectionName(header, name)) { if (!regular(path)) return false; continue; }
    const auto stage = temporary(name, header.kind == "runtime" ? ".stage-" : ".scripts-stage-");
    if (!stage || !directory(path, false)) return false;
  }
  return true;
}
} // namespace paleo::python::delivery
