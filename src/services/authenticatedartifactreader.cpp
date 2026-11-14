// 层：数据
#include "authenticatedartifactreader.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMutex>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace paleo::artifacts {
namespace {
constexpr int maxEntries = 8;
constexpr qint64 maxDigestBytes = 4 * 1024 * 1024;
constexpr int digestBytes = 32;
using Reader = AuthenticatedArtifactReader;
struct Index { QByteArray digests; };
struct Counters {
  std::atomic<quint64> fullScans{0}, fullHashBytes{0}, chunkHashBytes{0}, readBytes{0};
  std::atomic<quint64> cachedEntries{0}, cachedDigestBytes{0};
  Reader::Statistics snapshot() const {
    return {fullScans.load(), fullHashBytes.load(), chunkHashBytes.load(), readBytes.load(),
            cachedEntries.load(), cachedDigestBytes.load()};
  }
};
bool validIdentity(const Reader::Identity &identity) {
  if (identity.scopeKey.isEmpty() || identity.scopeKey.toUtf8().size() > 4096 ||
      identity.path.toUtf8().size() > 32768 || identity.path.contains(QChar::Null) ||
      !QFileInfo(identity.path).isAbsolute() ||
      identity.byteSize < 0 || identity.byteSize > Reader::MaxArtifactBytes || identity.sha256.size() != 64)
    return false;
  for (const auto c : identity.sha256) {
    const auto n = c.unicode();
    if (!((n >= '0' && n <= '9') || (n >= 'a' && n <= 'f') || (n >= 'A' && n <= 'F'))) return false;
  }
  return true;
}
QByteArray cacheKey(const Reader::Identity &identity) {
  // A length-delimited JSON array avoids separator collisions in caller-provided strings.
  return QJsonDocument(QJsonArray{identity.scopeKey, identity.path, identity.sha256.toLower(),
                                 QString::number(identity.byteSize)}).toJson(QJsonDocument::Compact);
}
bool shapeMatches(const Reader::Identity &identity, const QFile &file) {
  const QFileInfo info(identity.path);
  return info.isFile() && !info.isSymLink() && info.size() == identity.byteSize && file.size() == identity.byteSize;
}
QByteArray readChunk(QFile &file, qint64 offset, qint64 bytes, Counters &counts) {
  QByteArray chunk;
  if (!file.seek(offset)) return chunk;
  chunk.reserve(static_cast<qsizetype>(bytes));
  while (chunk.size() < bytes) {
    const auto part = file.read(bytes - chunk.size());
    if (part.isEmpty()) return {};
    counts.readBytes.fetch_add(static_cast<quint64>(part.size()));
    chunk.append(part);
  }
  return chunk;
}
QByteArray chunkDigest(const QByteArray &chunk, Counters &counts) {
  counts.chunkHashBytes.fetch_add(static_cast<quint64>(chunk.size()));
  return QCryptographicHash::hash(chunk, QCryptographicHash::Sha256);
}
void appendIntersection(QByteArray &page, const QByteArray &chunk, qint64 chunkOffset,
                        qint64 offset, qint64 count) {
  const auto begin = std::max(offset, chunkOffset);
  const auto end = std::min(offset + count, chunkOffset + chunk.size());
  if (begin < end) page.append(chunk.constData() + (begin - chunkOffset), static_cast<qsizetype>(end - begin));
}
}

struct AuthenticatedArtifactReader::Private {
  mutable QMutex mutex;
  std::atomic_bool invalidated{false};
  Counters counts;
  QHash<QByteArray, Index> indexes;
  QList<QByteArray> recency;
  qint64 retainedBytes = 0;
  bool stopped(const std::function<bool()> &cancelled) const {
    return invalidated.load() || (cancelled && cancelled());
  }
  void publishSize() {
    counts.cachedEntries.store(static_cast<quint64>(indexes.size()));
    counts.cachedDigestBytes.store(static_cast<quint64>(retainedBytes));
  }
  void clear() {
    indexes.clear(); recency.clear(); retainedBytes = 0; publishSize();
  }
  void erase(const QByteArray &key) {
    const auto it = indexes.find(key);
    if (it != indexes.end()) { retainedBytes -= it->digests.size(); indexes.erase(it); }
    recency.removeAll(key); publishSize();
  }
  void makeRoom(qint64 needed) {
    // Reserve space before building: even the temporary cold index fits the digest budget.
    while (!recency.isEmpty() && (indexes.size() >= maxEntries || retainedBytes + needed > maxDigestBytes)) {
      const auto oldest = recency.first(); erase(oldest);
    }
  }
  void retain(const QByteArray &key, Index index) {
    retainedBytes += index.digests.size(); indexes.insert(key, std::move(index));
    recency.removeAll(key); recency.append(key); publishSize();
  }
  Page scan(QFile &file, const Identity &identity, qint64 offset, qint64 wanted,
            const QByteArray &key, const std::function<bool()> &cancelled) {
    const auto chunks = (identity.byteSize + ChunkBytes - 1) / ChunkBytes;
    makeRoom(chunks * digestBytes);
    Index index; index.digests.reserve(static_cast<qsizetype>(chunks * digestBytes));
    QCryptographicHash hash(QCryptographicHash::Sha256); Page page;
    counts.fullScans.fetch_add(1);
    for (qint64 start = 0; start < identity.byteSize; start += ChunkBytes) {
      if (stopped(cancelled)) return {"cancelled", {}};
      const auto length = std::min(ChunkBytes, identity.byteSize - start);
      const auto chunk = readChunk(file, start, length, counts);
      if (chunk.size() != length) return {"artifact_read_failed", {}};
      hash.addData(chunk); counts.fullHashBytes.fetch_add(static_cast<quint64>(chunk.size()));
      index.digests.append(chunkDigest(chunk, counts));
      appendIntersection(page.bytes, chunk, start, offset, wanted);
      if (stopped(cancelled)) return {"cancelled", {}};
    }
    if (hash.result().toHex() != identity.sha256.toLower().toLatin1() || !shapeMatches(identity, file))
      return {"artifact_changed", {}};
    if (stopped(cancelled)) return {"cancelled", {}};
    retain(key, std::move(index)); page.code = "ok"; return page;
  }
  Page authenticate(QFile &file, const Identity &identity, qint64 offset, qint64 wanted,
                    const Index &index, const std::function<bool()> &cancelled) {
    Page page;
    if (identity.byteSize) {
      // EOF still authenticates its boundary chunk; empty files were fully bound to SHA(empty).
      const auto first = std::min(offset, identity.byteSize - 1) / ChunkBytes;
      const auto last = wanted ? (offset + wanted - 1) / ChunkBytes : first;
      for (auto n = first; n <= last; ++n) {
        if (stopped(cancelled)) return {"cancelled", {}};
        const auto start = n * ChunkBytes, length = std::min(ChunkBytes, identity.byteSize - start);
        const auto chunk = readChunk(file, start, length, counts);
        if (chunk.size() != length) return {"artifact_read_failed", {}};
        if (chunkDigest(chunk, counts) != index.digests.mid(static_cast<qsizetype>(n * digestBytes), digestBytes))
          return {"artifact_changed", {}};
        appendIntersection(page.bytes, chunk, start, offset, wanted);
      }
    }
    if (!shapeMatches(identity, file)) return {"artifact_changed", {}};
    if (stopped(cancelled)) return {"cancelled", {}};
    page.code = "ok"; return page;
  }
};

AuthenticatedArtifactReader::AuthenticatedArtifactReader() : d(std::make_unique<Private>()) {}
AuthenticatedArtifactReader::~AuthenticatedArtifactReader() = default;
AuthenticatedArtifactReader::Statistics AuthenticatedArtifactReader::statistics() const { return d->counts.snapshot(); }
void AuthenticatedArtifactReader::invalidate() {
  d->invalidated.store(true);
  if (d->mutex.tryLock()) { d->clear(); d->mutex.unlock(); }
}
AuthenticatedArtifactReader::Page AuthenticatedArtifactReader::read(const Identity &identity, qint64 offset,
    int maxBytes, const std::function<bool()> &cancelled) {
  if (d->stopped(cancelled)) return {"cancelled", {}};
  if (!validIdentity(identity)) return {"invalid_artifact_manifest", {}};
  if (offset < 0 || offset > identity.byteSize || maxBytes < 1 || maxBytes > 24 * 1024)
    return {"invalid_read_budget", {}};
  while (!d->mutex.tryLock(25)) if (d->stopped(cancelled)) return {"cancelled", {}};
  std::unique_lock<QMutex> lock(d->mutex, std::adopt_lock);
  if (d->stopped(cancelled)) { if (d->invalidated.load()) d->clear(); return {"cancelled", {}}; }
  const auto key = cacheKey(identity); QFile file(identity.path);
  const auto info = QFileInfo(identity.path);
  if (!info.isFile() || info.isSymLink() || info.size() != identity.byteSize) {
    d->erase(key); return {"artifact_changed", {}};
  }
  if (!file.open(QIODevice::ReadOnly)) { d->erase(key); return {"artifact_read_failed", {}}; }
  if (!shapeMatches(identity, file)) { d->erase(key); return {"artifact_changed", {}}; }
  const auto wanted = std::min<qint64>(maxBytes, identity.byteSize - offset);
  const auto it = d->indexes.constFind(key);
  auto page = it == d->indexes.cend() ? d->scan(file, identity, offset, wanted, key, cancelled)
                                     : d->authenticate(file, identity, offset, wanted, it.value(), cancelled);
  if (page.code == QLatin1String("ok")) { d->recency.removeAll(key); d->recency.append(key); }
  else if (page.code != QLatin1String("cancelled")) d->erase(key);
  if (d->stopped(cancelled)) {
    if (d->invalidated.load()) d->clear();
    return {"cancelled", {}};
  }
  return page;
}
}
