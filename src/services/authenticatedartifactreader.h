// 层：数据
// Authenticates returned ranges against an in-memory index anchored to a full SHA-256.
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

namespace paleo::artifacts {
class AuthenticatedArtifactReader {
public:
  static constexpr qint64 ChunkBytes = 64 * 1024;
  static constexpr qint64 MaxArtifactBytes = 1024LL * 1024 * 1024;
  struct Identity {
    QString scopeKey, path, sha256;
    qint64 byteSize = -1;
  };
  struct Page { QString code; QByteArray bytes; };
  struct Statistics {
    quint64 fullScans = 0, fullHashBytes = 0, chunkHashBytes = 0, readBytes = 0;
    quint64 cachedEntries = 0, cachedDigestBytes = 0;
  };
  AuthenticatedArtifactReader();
  ~AuthenticatedArtifactReader();
  AuthenticatedArtifactReader(const AuthenticatedArtifactReader &) = delete;
  AuthenticatedArtifactReader &operator=(const AuthenticatedArtifactReader &) = delete;
  // Worker-only I/O. Concurrent cold requests share a scan; waiting is cancellable.
  // A successful page authenticates these bytes, not the health of unrequested ranges.
  Page read(const Identity &identity, qint64 offset, int maxBytes,
            const std::function<bool()> &cancelled = {});
  // Permanently invalidates this instance, including work already in flight. No I/O wait.
  void invalidate();
  // Lock-free diagnostic counters; concurrent fields need not form a transactional snapshot.
  Statistics statistics() const;
private:
  struct Private;
  std::unique_ptr<Private> d;
};
}
