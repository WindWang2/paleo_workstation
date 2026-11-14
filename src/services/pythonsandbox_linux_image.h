// 层：数据
#pragma once
#include "services/pythonruntime.h"
#include "services/pythoninputbundle.h"
#include <QVector>

namespace paleo::python {
struct SealedImageFile {
  QString path;
  int fd=-1;
  qint64 size=0;
  bool executable=false;
};
struct SealedRuntimeImage {
  QVector<SealedImageFile> files;
  qint64 bytes=0;
  QByteArray manifestBytes;
  QList<QByteArray> stdinChunks;
  qint64 bufferedBytes=0;
  std::shared_ptr<const VerifiedRuntime> runtime;
  ~SealedRuntimeImage();
};
// A host manifest SHA authenticates every copied byte. No path/stat success cache.
std::shared_ptr<SealedRuntimeImage> snapshotLinuxRuntime(std::shared_ptr<const VerifiedRuntime> runtime,qint64 capacity,
    const std::function<bool()> &cancelled,QString &code);
std::shared_ptr<SealedRuntimeImage> snapshotLinuxInputs(const InputBundle &input,const ExecutionPolicy &policy,qint64 capacity,qint64 bufferedCapacity,
    const std::function<bool()> &cancelled,QString &code);
}
