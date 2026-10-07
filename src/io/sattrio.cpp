// 层：数据
#include "io/sattrio.h"
#include "ioerrors_internal.h"

#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>

namespace paleo::sattr
{

namespace
{

using paleo::io_detail::setError;

// 小端 f32 块的快路径：小端宿主直写内存像，大端逐值换序（正确性优先，
// 热路径在主流小端平台零拷贝）。
bool writeFloatBlock(QIODevice &file, const float *values, qint64 count)
{
  if (count <= 0)
    return true;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
  return file.write(reinterpret_cast<const char *>(values),
                    std::size_t(count) * sizeof(float)) ==
         std::size_t(count) * sizeof(float);
#else
  std::vector<quint32> swapped(std::size_t(count));
  for (qint64 i = 0; i < count; ++i)
  {
    quint32 raw;
    std::memcpy(&raw, values + i, sizeof(raw));
    swapped[std::size_t(i)] = qToLittleEndian(raw);
  }
  return file.write(reinterpret_cast<const char *>(swapped.data()),
                    swapped.size() * sizeof(quint32)) ==
         qint64(swapped.size() * sizeof(quint32));
#endif
}

bool readFloatBlock(QDataStream &stream, float *values, qint64 count)
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
  return stream.readRawData(reinterpret_cast<char *>(values),
                            int(std::min<qint64>(count * 4, std::numeric_limits<int>::max()))) ==
         int(count) * 4;
#else
  for (qint64 i = 0; i < count; ++i)
    stream >> values[i];
  return stream.status() == QDataStream::Ok;
#endif
}

QJsonObject sectionHeaderJson(const SattrSectionHeader &h)
{
  QJsonObject header;
  header.insert(QStringLiteral("attrId"), h.attrId);
  header.insert(QStringLiteral("section"), h.section);
  header.insert(QStringLiteral("sectionIndex"), h.sectionIndex);
  header.insert(QStringLiteral("width"), h.width);
  header.insert(QStringLiteral("height"), h.height);
  header.insert(QStringLiteral("valueMin"), h.valueMin);
  header.insert(QStringLiteral("valueMax"), h.valueMax);
  header.insert(QStringLiteral("traceCount"), double(h.traceCount));
  header.insert(QStringLiteral("validTraceCount"), double(h.validTraceCount));
  header.insert(QStringLiteral("readMs"), h.readMs);
  header.insert(QStringLiteral("computeMs"), h.computeMs);
  header.insert(QStringLiteral("sourceSgyPath"), h.sourceSgyPath);
  header.insert(QStringLiteral("createdAt"), h.createdAt);
  QJsonObject p;
  p.insert(QStringLiteral("windowHalfSamples"), h.windowHalfSamples);
  p.insert(QStringLiteral("coherenceIlHalf"), h.coherenceIlHalf);
  p.insert(QStringLiteral("coherenceXlHalf"), h.coherenceXlHalf);
  p.insert(QStringLiteral("coherenceTimeHalf"), h.coherenceTimeHalf);
  p.insert(QStringLiteral("coherenceWeighting"), h.coherenceWeighting);
  header.insert(QStringLiteral("params"), p);
  return header;
}

void fillSectionHeader(const QJsonObject &header, SattrSectionHeader *h)
{
  h->attrId = header.value(QStringLiteral("attrId")).toString();
  h->section = header.value(QStringLiteral("section")).toString();
  h->sectionIndex = header.value(QStringLiteral("sectionIndex")).toInt();
  h->valueMin = header.value(QStringLiteral("valueMin")).toDouble();
  h->valueMax = header.value(QStringLiteral("valueMax")).toDouble();
  h->traceCount = qint64(header.value(QStringLiteral("traceCount")).toDouble());
  h->validTraceCount =
      qint64(header.value(QStringLiteral("validTraceCount")).toDouble());
  h->readMs = header.value(QStringLiteral("readMs")).toDouble();
  h->computeMs = header.value(QStringLiteral("computeMs")).toDouble();
  h->sourceSgyPath = header.value(QStringLiteral("sourceSgyPath")).toString();
  h->createdAt = header.value(QStringLiteral("createdAt")).toString();
  const QJsonObject p = header.value(QStringLiteral("params")).toObject();
  h->windowHalfSamples = p.value(QStringLiteral("windowHalfSamples")).toInt(8);
  h->coherenceIlHalf = p.value(QStringLiteral("coherenceIlHalf")).toInt(1);
  h->coherenceXlHalf = p.value(QStringLiteral("coherenceXlHalf")).toInt(1);
  h->coherenceTimeHalf = p.value(QStringLiteral("coherenceTimeHalf")).toInt(2);
  h->coherenceWeighting = p.value(QStringLiteral("coherenceWeighting")).toInt(0);
}

constexpr quint32 kSectionVersion = 1;
constexpr quint32 kVolumeVersion = 1;
constexpr int kMaxJsonBytes = 4 * 1024 * 1024;

} // namespace

// ---- SATR 剖面容器 -----------------------------------------------------------

bool writeSattrSection(const QString &path, const SattrSectionHeader &header,
                       const QVector<float> &values, QString *error)
{
  if (header.width <= 0 || header.height <= 0 ||
      header.section != QStringLiteral("il") && header.section != QStringLiteral("xl"))
  {
    setError(error, QStringLiteral("SATR 头几何/方向无效"));
    return false;
  }
  if (values.size() != qint64(header.width) * header.height)
  {
    setError(error, QStringLiteral("SATR 值块尺寸与头不符（%1 != %2×%3）")
                          .arg(values.size()).arg(header.width).arg(header.height));
    return false;
  }
  const QByteArray json =
      QJsonDocument(sectionHeaderJson(header)).toJson(QJsonDocument::Compact);
  // #233：QSaveFile 暂存 + commit 发布——写中途失败/崩溃不留截断 .satr，
  // 旧文件保持完好（对齐 SattrVolumeWriter 失败即删与 QSaveFile 口径）。
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly))
  {
    setError(error, QStringLiteral("无法写属性文件 %1").arg(path));
    return false;
  }
  QDataStream ds(&f);
  ds.setByteOrder(QDataStream::LittleEndian);
  ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
  ds.writeRawData("SATR", 4);
  ds << kSectionVersion << qint32(header.width) << qint32(header.height)
     << quint32(json.size());
  ds.writeRawData(json.constData(), json.size());
  if (ds.status() != QDataStream::Ok ||
      !writeFloatBlock(f, values.constData(), values.size()))
  {
    setError(error, QStringLiteral("SATR 值块写入失败：%1（%2）")
                             .arg(path, f.errorString()));
    f.cancelWriting();
    return false;
  }
  if (!f.commit())
  {
    setError(error, QStringLiteral("SATR 发布失败：%1（%2）").arg(path, f.errorString()));
    return false;
  }
  return true;
}

bool readSattrSection(const QString &path, SattrSectionHeader *header,
                      QVector<float> *values, QString *error,
                      const std::function<bool()> &cancelled)
{
  auto fail = [&](const QString &e) {
    setError(error, e);
    return false;
  };
  if (!header || !values)
    return fail(QStringLiteral("SATR 读回参数为空"));
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("SATR 文件无法打开：%1").arg(path));
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  char magic[4];
  quint32 version = 0, jsonSize = 0;
  qint32 width = 0, height = 0;
  if (stream.readRawData(magic, 4) != 4 || QByteArray(magic, 4) != "SATR")
    return fail(QStringLiteral("SATR 魔数无效：%1").arg(path));
  stream >> version >> width >> height >> jsonSize;
  if (version != kSectionVersion || width <= 0 || height <= 0 ||
      qint64(width) * height > std::numeric_limits<int>::max() / 4 ||
      jsonSize == 0 || jsonSize > kMaxJsonBytes ||
      file.size() != 20 + qint64(jsonSize) + qint64(width) * height * 4)
    return fail(QStringLiteral("SATR 版本或载荷大小无效：%1").arg(path));
  QByteArray json(int(jsonSize), Qt::Uninitialized);
  if (stream.readRawData(json.data(), json.size()) != json.size())
    return fail(QStringLiteral("SATR 头截断：%1").arg(path));
  QJsonParseError parseError;
  const QJsonObject headerJson =
      QJsonDocument::fromJson(json, &parseError).object();
  if (parseError.error != QJsonParseError::NoError)
    return fail(QStringLiteral("SATR JSON 无效：%1").arg(path));
  fillSectionHeader(headerJson, header);
  header->width = width;
  header->height = height;
  if (header->section != QStringLiteral("il") && header->section != QStringLiteral("xl"))
    return fail(QStringLiteral("SATR 剖面方向不支持：%1").arg(header->section));

  values->resize(std::size_t(width) * height);
  if (!readFloatBlock(stream, values->data(), values->size()))
    return fail(QStringLiteral("SATR 值块截断：%1").arg(path));
  if (cancelled && !cancelled())
  {
    // 与 crossplot 旧读端同语义：协作取消发生在读值途中 → 如实报已取消。
    return fail(QStringLiteral("SATR 读回已取消"));
  }
  return true;
}

// ---- SATV 体容器：写端 -------------------------------------------------------

SattrVolumeWriter::~SattrVolumeWriter()
{
  if (file_.isOpen())
    file_.close(); // 未 finish 的半成品不补头——调用方按失败处理
}

bool SattrVolumeWriter::begin(const QString &path, const SattrVolumeInfo &info,
                              QString *error)
{
  if (info.nIl <= 0 || info.nXl <= 0 || info.nS <= 0 || info.blockIl <= 0)
  {
    setError(error, QStringLiteral("SATV 头几何无效（nIl/nXl/nS/blockIl 须为正）"));
    return false;
  }
  if (info.ilValues.size() != info.nIl || info.xlValues.size() != info.nXl)
  {
    setError(error, QStringLiteral("SATV 轴值表长度与几何不符"));
    return false;
  }
  if (qint64(info.blockIl) * info.nXl * info.nS > qint64(256) * 1024 * 1024 / 4)
  {
    setError(error, QStringLiteral("SATV 单块超 256MiB（blockIl 过大）"));
    return false;
  }
  info_ = info;
  info_.blockCount = (info.nIl + info.blockIl - 1) / info.blockIl;

  QJsonArray ilArr, xlArr;
  for (int v : info.ilValues)
    ilArr.append(v);
  for (int v : info.xlValues)
    xlArr.append(v);
  QJsonObject header;
  header.insert(QStringLiteral("attrId"), info.attrId);
  header.insert(QStringLiteral("kind"), QStringLiteral("volume"));
  header.insert(QStringLiteral("nIl"), info.nIl);
  header.insert(QStringLiteral("nXl"), info.nXl);
  header.insert(QStringLiteral("nS"), info.nS);
  header.insert(QStringLiteral("ilValues"), ilArr);
  header.insert(QStringLiteral("xlValues"), xlArr);
  header.insert(QStringLiteral("sampleIntervalMs"), info.sampleIntervalMs);
  header.insert(QStringLiteral("startTimeMs"), info.startTimeMs);
  header.insert(QStringLiteral("blockIl"), info.blockIl);
  header.insert(QStringLiteral("blockCount"), info_.blockCount);
  header.insert(QStringLiteral("readMs"), info.readMs);
  header.insert(QStringLiteral("computeMs"), info.computeMs);
  header.insert(QStringLiteral("sourceSgyPath"), info.sourceSgyPath);
  header.insert(QStringLiteral("createdAt"), info.createdAt);
  header.insert(QStringLiteral("paramHash"), info.paramHash);
  header.insert(QStringLiteral("ilTraceSpacing"), info.ilTraceSpacing);
  header.insert(QStringLiteral("xlTraceSpacing"), info.xlTraceSpacing);
  QJsonObject p;
  p.insert(QStringLiteral("windowHalfSamples"), info.windowHalfSamples);
  p.insert(QStringLiteral("coherenceIlHalf"), info.coherenceIlHalf);
  p.insert(QStringLiteral("coherenceXlHalf"), info.coherenceXlHalf);
  p.insert(QStringLiteral("coherenceTimeHalf"), info.coherenceTimeHalf);
  p.insert(QStringLiteral("coherenceWeighting"), info.coherenceWeighting);
  header.insert(QStringLiteral("params"), p);
  const QByteArray json =
      QJsonDocument(header).toJson(QJsonDocument::Compact);

  block_.clear();
  block_.reserve(std::size_t(info.blockIl) * info.nXl * info.nS);
  writtenIl_ = 0;

  if (QFileInfo::exists(path) && !QFile::remove(path))
  {
    setError(error, QStringLiteral("无法覆盖既有属性体文件 %1").arg(path));
    return false;
  }
  file_.setFileName(path);
  if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("无法写属性体文件 %1").arg(path));
    return false;
  }
  QDataStream ds(&file_);
  ds.setByteOrder(QDataStream::LittleEndian);
  ds.writeRawData("SATV", 4);
  ds << kVolumeVersion << quint32(json.size());
  ds.writeRawData(json.constData(), json.size());

  // 块表：偏移/尺寸前置可算（payload 定长，末块短），一次写全——无需回填。
  const qint64 tableStart = 12 + json.size() + 4 + qint64(16) * info_.blockCount;
  ds << quint32(info_.blockCount);
  qint64 offset = tableStart;
  for (int b = 0; b < info_.blockCount; ++b)
  {
    const int rows = std::min(info.blockIl, info.nIl - b * info.blockIl);
    const qint64 bytes = qint64(rows) * info.nXl * info.nS * 4;
    ds << quint64(offset) << quint64(bytes);
    offset += bytes;
  }
  if (ds.status() != QDataStream::Ok)
  {
    setError(error, QStringLiteral("SATV 头/块表写入失败：%1（%2）")
                             .arg(path, file_.errorString()));
    file_.close();
    QFile::remove(path); // begin 失败不留截断残件
    return false;
  }
  return true;
}

bool SattrVolumeWriter::flushBlock(QString *error)
{
  if (block_.empty())
    return true;
  if (!writeFloatBlock(file_, block_.data(), qint64(block_.size())))
  {
    setError(error, QStringLiteral("SATV payload 写入失败：%1（%2）")
                             .arg(file_.fileName(), file_.errorString()));
    return false;
  }
  block_.clear();
  return true;
}

bool SattrVolumeWriter::writeInline(const float *values, QString *error)
{
  if (!file_.isOpen())
  {
    setError(error, QStringLiteral("SATV 写端未 begin"));
    return false;
  }
  if (writtenIl_ >= info_.nIl)
  {
    setError(error, QStringLiteral("SATV 写端越界（已写 %1 条 inline）").arg(writtenIl_));
    return false;
  }
  const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
  block_.insert(block_.end(), values, values + traceLen);
  ++writtenIl_;
  if ((writtenIl_ % info_.blockIl) == 0)
    return flushBlock(error);
  return true;
}

void SattrVolumeWriter::abort()
{
  if (file_.isOpen())
    file_.close();
  block_.clear();
  writtenIl_ = 0;
}

bool SattrVolumeWriter::finish(double valueMin, double valueMax,
                               qint64 validCells, QString *error)
{
  if (!file_.isOpen())
  {
    setError(error, QStringLiteral("SATV 写端未 begin"));
    return false;
  }
  if (writtenIl_ != info_.nIl)
  {
    setError(error, QStringLiteral("SATV 未写满（%1/%2 条 inline）")
                        .arg(writtenIl_).arg(info_.nIl));
    file_.close();
    return false;
  }
  if (!flushBlock(error))
    return false;
  // EOF footer：统计值在扫描尾部才可知（头不预写假值）。
  char footer[24];
  qToLittleEndian<double>(valueMin, footer);
  qToLittleEndian<double>(valueMax, footer + 8);
  qToLittleEndian<quint64>(quint64(validCells), footer + 16);
  if (file_.write(footer, sizeof(footer)) != sizeof(footer))
  {
    setError(error, QStringLiteral("SATV footer 写入失败：%1").arg(file_.fileName()));
    file_.close();
    return false;
  }
  file_.flush();
  file_.close();
  return true;
}

// ---- SATV 体容器：读端 -------------------------------------------------------

bool SattrVolumeReader::open(const QString &path, QString *error)
{
  auto fail = [&](const QString &e) {
    setError(error, e);
    return false;
  };
  file_.setFileName(path);
  if (!file_.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("SATV 文件无法打开：%1").arg(path));
  QDataStream stream(&file_);
  stream.setByteOrder(QDataStream::LittleEndian);
  char magic[4];
  quint32 version = 0, jsonSize = 0;
  if (stream.readRawData(magic, 4) != 4 || QByteArray(magic, 4) != "SATV")
    return fail(QStringLiteral("SATV 魔数无效：%1").arg(path));
  stream >> version >> jsonSize;
  if (version != kVolumeVersion || jsonSize == 0 || jsonSize > kMaxJsonBytes)
    return fail(QStringLiteral("SATV 版本/头长无效：%1").arg(path));
  QByteArray json(int(jsonSize), Qt::Uninitialized);
  if (stream.readRawData(json.data(), json.size()) != json.size())
    return fail(QStringLiteral("SATV 头截断：%1").arg(path));
  QJsonParseError parseError;
  const QJsonObject header =
      QJsonDocument::fromJson(json, &parseError).object();
  if (parseError.error != QJsonParseError::NoError)
    return fail(QStringLiteral("SATV JSON 无效：%1").arg(path));
  if (header.value(QStringLiteral("kind")).toString() != QStringLiteral("volume"))
    return fail(QStringLiteral("SATV 头缺 volume 标记：%1").arg(path));

  SattrVolumeInfo info;
  info.attrId = header.value(QStringLiteral("attrId")).toString();
  info.nIl = header.value(QStringLiteral("nIl")).toInt();
  info.nXl = header.value(QStringLiteral("nXl")).toInt();
  info.nS = header.value(QStringLiteral("nS")).toInt();
  info.sampleIntervalMs = header.value(QStringLiteral("sampleIntervalMs")).toDouble();
  info.startTimeMs = header.value(QStringLiteral("startTimeMs")).toDouble();
  info.blockIl = header.value(QStringLiteral("blockIl")).toInt();
  info.blockCount = header.value(QStringLiteral("blockCount")).toInt();
  info.readMs = header.value(QStringLiteral("readMs")).toDouble();
  info.computeMs = header.value(QStringLiteral("computeMs")).toDouble();
  info.sourceSgyPath = header.value(QStringLiteral("sourceSgyPath")).toString();
  info.createdAt = header.value(QStringLiteral("createdAt")).toString();
  info.paramHash = header.value(QStringLiteral("paramHash")).toString();
  info.ilTraceSpacing = header.value(QStringLiteral("ilTraceSpacing")).toDouble();
  info.xlTraceSpacing = header.value(QStringLiteral("xlTraceSpacing")).toDouble();
  const QJsonObject p = header.value(QStringLiteral("params")).toObject();
  info.windowHalfSamples = p.value(QStringLiteral("windowHalfSamples")).toInt(8);
  info.coherenceIlHalf = p.value(QStringLiteral("coherenceIlHalf")).toInt(1);
  info.coherenceXlHalf = p.value(QStringLiteral("coherenceXlHalf")).toInt(1);
  info.coherenceTimeHalf = p.value(QStringLiteral("coherenceTimeHalf")).toInt(2);
  info.coherenceWeighting = p.value(QStringLiteral("coherenceWeighting")).toInt(0);
  const QJsonArray ilArr = header.value(QStringLiteral("ilValues")).toArray();
  const QJsonArray xlArr = header.value(QStringLiteral("xlValues")).toArray();
  for (const auto &v : ilArr)
    info.ilValues.append(v.toInt());
  for (const auto &v : xlArr)
    info.xlValues.append(v.toInt());

  if (info.nIl <= 0 || info.nXl <= 0 || info.nS <= 0 || info.blockIl <= 0 ||
      info.ilValues.size() != info.nIl || info.xlValues.size() != info.nXl ||
      qint64(info.nIl) * info.nXl * info.nS > std::numeric_limits<int>::max())
    return fail(QStringLiteral("SATV 头几何/轴值表无效：%1").arg(path));
  const int expectBlocks = (info.nIl + info.blockIl - 1) / info.blockIl;
  if (info.blockCount != expectBlocks)
    return fail(QStringLiteral("SATV 块数与几何不符（%1 != %2）")
                    .arg(info.blockCount).arg(expectBlocks));

  quint32 blockCount = 0;
  stream >> blockCount;
  if (blockCount != quint32(expectBlocks))
    return fail(QStringLiteral("SATV 块表条数无效：%1").arg(path));
  blocks_.resize(std::size_t(blockCount));
  const qint64 fileSize = file_.size();
  const qint64 tableStart = 12 + json.size() + 4;
  const qint64 payloadStart = tableStart + qint64(16) * blockCount;
  qint64 offset = payloadStart;
  for (quint32 b = 0; b < blockCount; ++b)
  {
    const int rows = std::min(info.blockIl, info.nIl - int(b) * info.blockIl);
    const qint64 bytes = qint64(rows) * info.nXl * info.nS * 4;
    stream >> blocks_[std::size_t(b)].offset >> blocks_[std::size_t(b)].bytes;
    if (blocks_[std::size_t(b)].offset != quint64(offset) ||
        blocks_[std::size_t(b)].bytes != quint64(bytes) ||
        offset + bytes > fileSize)
      return fail(QStringLiteral("SATV 块表寻址与文件不符：%1（块 %2）")
                      .arg(path).arg(b));
    offset += bytes;
  }
  // EOF footer（24B：值域/有效单元统计；截断即整体不自洽）。
  constexpr qint64 kFooterBytes = 24;
  if (fileSize != offset + kFooterBytes)
    return fail(QStringLiteral("SATV 文件长与块表+footer 不符：%1（差 %2 字节）")
                    .arg(path).arg(fileSize - offset - kFooterBytes));
  char footer[24];
  if (!file_.seek(offset) ||
      file_.read(footer, sizeof(footer)) != sizeof(footer))
    return fail(QStringLiteral("SATV footer 读取失败：%1").arg(path));
  info.valueMin = qFromLittleEndian<double>(footer);
  info.valueMax = qFromLittleEndian<double>(footer + 8);
  info.validCells = qint64(qFromLittleEndian<quint64>(footer + 16));
  info_ = std::move(info);
  blockScratch_.clear();
  return true;
}

bool SattrVolumeReader::readBlock(int blockIndex, QString *error)
{
  if (blockIndex < 0 || blockIndex >= int(blocks_.size()))
  {
    setError(error, QStringLiteral("SATV 块索引越界（%1）").arg(blockIndex));
    return false;
  }
  const int rows =
      std::min(info_.blockIl, info_.nIl - blockIndex * info_.blockIl);
  const qint64 floats = qint64(rows) * info_.nXl * info_.nS;
  blockScratch_.resize(std::size_t(floats));
  if (!file_.seek(qint64(blocks_[std::size_t(blockIndex)].offset)))
  {
    setError(error, QStringLiteral("SATV 块寻址失败（块 %1）").arg(blockIndex));
    return false;
  }
  QDataStream stream(&file_);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  if (!readFloatBlock(stream, blockScratch_.data(), floats))
  {
    setError(error, QStringLiteral("SATV 块载荷截断（块 %1）").arg(blockIndex));
    return false;
  }
  return true;
}

bool SattrVolumeReader::extractInline(int ilIdx, std::vector<float> *out,
                                      QString *error)
{
  if (!isOpen() || !out)
  {
    setError(error, QStringLiteral("SATV 读端未打开"));
    return false;
  }
  if (ilIdx < 0 || ilIdx >= info_.nIl)
  {
    setError(error, QStringLiteral("SATV inline 索引越界（%1/%2）").arg(ilIdx).arg(info_.nIl));
    return false;
  }
  const int blockIndex = ilIdx / info_.blockIl;
  const int localIl = ilIdx - blockIndex * info_.blockIl;
  if (!readBlock(blockIndex, error))
    return false;
  const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
  out->assign(blockScratch_.begin() + std::ptrdiff_t(localIl) * traceLen,
              blockScratch_.begin() + std::ptrdiff_t(localIl + 1) * traceLen);
  return true;
}

bool SattrVolumeReader::extractXline(int xlIdx, std::vector<float> *out,
                                     QString *error)
{
  if (!isOpen() || !out)
  {
    setError(error, QStringLiteral("SATV 读端未打开"));
    return false;
  }
  if (xlIdx < 0 || xlIdx >= info_.nXl)
  {
    setError(error, QStringLiteral("SATV crossline 索引越界（%1/%2）").arg(xlIdx).arg(info_.nXl));
    return false;
  }
  const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
  out->assign(std::size_t(info_.nIl) * info_.nS, std::numeric_limits<float>::quiet_NaN());
  for (int b = 0; b < info_.blockCount; ++b)
  {
    const int rows = std::min(info_.blockIl, info_.nIl - b * info_.blockIl);
    if (!readBlock(b, error))
      return false;
    for (int r = 0; r < rows; ++r)
    {
      const int ilIdx = b * info_.blockIl + r;
      for (int s = 0; s < info_.nS; ++s)
        (*out)[std::size_t(ilIdx) * info_.nS + std::size_t(s)] =
            blockScratch_[r * traceLen + std::size_t(xlIdx) * info_.nS + std::size_t(s)];
    }
  }
  return true;
}

bool SattrVolumeReader::extractTime(int sIdx, std::vector<float> *out,
                                    QString *error)
{
  if (!isOpen() || !out)
  {
    setError(error, QStringLiteral("SATV 读端未打开"));
    return false;
  }
  if (sIdx < 0 || sIdx >= info_.nS)
  {
    setError(error, QStringLiteral("SATV 采样索引越界（%1/%2）").arg(sIdx).arg(info_.nS));
    return false;
  }
  out->assign(std::size_t(info_.nIl) * info_.nXl,
              std::numeric_limits<float>::quiet_NaN());
  for (int b = 0; b < info_.blockCount; ++b)
  {
    const int rows = std::min(info_.blockIl, info_.nIl - b * info_.blockIl);
    if (!readBlock(b, error))
      return false;
    const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
    for (int r = 0; r < rows; ++r)
    {
      const int ilIdx = b * info_.blockIl + r;
      for (int xl = 0; xl < info_.nXl; ++xl)
        (*out)[std::size_t(ilIdx) * info_.nXl + std::size_t(xl)] =
            blockScratch_[std::size_t(r) * traceLen + std::size_t(xl) * info_.nS +
                          std::size_t(sIdx)];
    }
  }
  return true;
}

bool SattrVolumeReader::extractTimePlanes(const std::vector<int> &sIdx,
                                          std::vector<std::vector<float>> *out,
                                          QString *error)
{
  if (!isOpen() || !out)
  {
    setError(error, QStringLiteral("SATV 读端未打开"));
    return false;
  }
  for (int s : sIdx)
    if (s < 0 || s >= info_.nS)
    {
      setError(error, QStringLiteral("SATV 采样索引越界（%1/%2）").arg(s).arg(info_.nS));
      return false;
    }
  out->assign(sIdx.size(),
              std::vector<float>(std::size_t(info_.nIl) * info_.nXl,
                                 std::numeric_limits<float>::quiet_NaN()));
  const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
  for (int b = 0; b < info_.blockCount; ++b)
  {
    const int rows = std::min(info_.blockIl, info_.nIl - b * info_.blockIl);
    if (!readBlock(b, error))
      return false;
    for (int r = 0; r < rows; ++r)
    {
      const int ilIdx = b * info_.blockIl + r;
      for (int xl = 0; xl < info_.nXl; ++xl)
      {
        const float *trace =
            blockScratch_.data() + std::size_t(r) * traceLen +
            std::size_t(xl) * info_.nS;
        for (std::size_t k = 0; k < sIdx.size(); ++k)
          (*out)[k][std::size_t(ilIdx) * info_.nXl + std::size_t(xl)] =
              trace[std::size_t(sIdx[k])];
      }
    }
  }
  return true;
}

bool SattrVolumeReader::readCell(int ilIdx, int xlIdx, int sIdx, float *out,
                                 QString *error)
{
  if (!isOpen() || !out)
  {
    setError(error, QStringLiteral("SATV 读端未打开"));
    return false;
  }
  if (ilIdx < 0 || ilIdx >= info_.nIl || xlIdx < 0 || xlIdx >= info_.nXl ||
      sIdx < 0 || sIdx >= info_.nS)
  {
    setError(error, QStringLiteral("SATV 单元索引越界（%1,%2,%3）").arg(ilIdx).arg(xlIdx).arg(sIdx));
    return false;
  }
  if (!readBlock(ilIdx / info_.blockIl, error))
    return false;
  const int localIl = ilIdx - (ilIdx / info_.blockIl) * info_.blockIl;
  const std::size_t traceLen = std::size_t(info_.nXl) * info_.nS;
  *out = blockScratch_[std::size_t(localIl) * traceLen + std::size_t(xlIdx) * info_.nS +
                       std::size_t(sIdx)];
  return true;
}

} // namespace paleo::sattr
