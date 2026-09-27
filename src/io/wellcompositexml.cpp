#include "wellcompositexml.h"

#include <QFile>
#include <QHash>
#include <QXmlStreamReader>
#include <cmath>
#include <limits>

namespace WellComposite
{

bool ComprehensiveWellData::isEmpty() const
{
  return continuousCurves.isEmpty() && discreteCurves.isEmpty() &&
         lithologyIntervals.isEmpty() && formationIntervals.isEmpty();
}

static QColor pickWellCurveColor(const QString &mnemonic, int index)
{
  const QString upper = mnemonic.toUpper();
  if (upper.contains(QStringLiteral("GR")))
    return QColor(QStringLiteral("#2E7D32")); // 绿色
  if (upper.contains(QStringLiteral("AC")) || upper.contains(QStringLiteral("DT")) || upper.contains(QStringLiteral("RACELM")))
    return QColor(QStringLiteral("#0288D1")); // 蓝色
  if (upper.contains(QStringLiteral("DEN")) || upper.contains(QStringLiteral("ZDEN")) || upper.contains(QStringLiteral("RHOB")))
    return QColor(QStringLiteral("#D32F2F")); // 红色
  if (upper.contains(QStringLiteral("CN")) || upper.contains(QStringLiteral("NPHI")))
    return QColor(QStringLiteral("#E65100")); // 橙色
  if (upper.contains(QStringLiteral("RT")) || upper.contains(QStringLiteral("RD")) || upper.contains(QStringLiteral("RPC")))
    return QColor(QStringLiteral("#7B1FA2")); // 紫色
  if (upper.contains(QStringLiteral("CAL")))
    return QColor(QStringLiteral("#455A64")); // 石板灰
  if (upper.contains(QStringLiteral("SP")))
    return QColor(QStringLiteral("#C2185B")); // 品红
  if (upper.contains(QStringLiteral("POR")))
    return QColor(QStringLiteral("#00897B")); // 蓝绿
  if (upper.contains(QStringLiteral("PERM")) || upper.contains(QStringLiteral("KAR")))
    return QColor(QStringLiteral("#FB8C00")); // 琥珀
  if (upper.contains(QStringLiteral("TOC")))
    return QColor(QStringLiteral("#5D4037")); // 棕褐

  static const QVector<QColor> fallbackColors = {
      QColor(QStringLiteral("#1B73D0")), QColor(QStringLiteral("#D96B27")),
      QColor(QStringLiteral("#0E8A6E")), QColor(QStringLiteral("#8E44AD")),
      QColor(QStringLiteral("#C0392B")), QColor(QStringLiteral("#16A085"))};
  return fallbackColors.at(index % fallbackColors.size());
}

static QString pickUnitForMnemonic(const QString &mnemonic)
{
  const QString upper = mnemonic.toUpper();
  if (upper.contains(QStringLiteral("GR"))) return QStringLiteral("API");
  if (upper.contains(QStringLiteral("AC")) || upper.contains(QStringLiteral("DT"))) return QStringLiteral("μs/ft");
  if (upper.contains(QStringLiteral("DEN")) || upper.contains(QStringLiteral("ZDEN"))) return QStringLiteral("g/cm³");
  if (upper.contains(QStringLiteral("CNCF")) || upper.contains(QStringLiteral("NPHI")) || upper.contains(QStringLiteral("POR"))) return QStringLiteral("%");
  if (upper.contains(QStringLiteral("RPC")) || upper.contains(QStringLiteral("RAC")) || upper.contains(QStringLiteral("RT")) || upper.contains(QStringLiteral("RD"))) return QStringLiteral("Ω·m");
  if (upper.contains(QStringLiteral("CAL"))) return QStringLiteral("in");
  if (upper.contains(QStringLiteral("KAR")) || upper.contains(QStringLiteral("PERM"))) return QStringLiteral("mD");
  return QString();
}

bool parseComprehensiveWellXml(const QString &filePath, ComprehensiveWellData &outData, QString *errorMsg)
{
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly))
  {
    if (errorMsg) *errorMsg = QStringLiteral("无法打开文件: %1").arg(filePath);
    return false;
  }
  return parseComprehensiveWellXmlData(file.readAll(), outData, errorMsg);
}

bool parseComprehensiveWellXmlData(const QByteArray &content, ComprehensiveWellData &outData, QString *errorMsg)
{
  QXmlStreamReader xml(content);

  QString currentSheetName;
  QVector<QStringList> sheetRows;
  sheetRows.reserve(512);

  // 解析并分流单个工作表
  const auto processSheet = [&](const QString &sheetName, const QVector<QStringList> &rows) {
    if (rows.isEmpty()) return;

    if (sheetName.startsWith(QStringLiteral("测井曲线")))
    {
      // 测井曲线表
      const QStringList &header = rows.first();
      int depthCol = 1;
      for (int c = 0; c < header.size(); ++c)
      {
        if (header.at(c).trimmed() == QStringLiteral("深度") || header.at(c).trimmed().toUpper() == QStringLiteral("DEPT") || header.at(c).trimmed().toUpper() == QStringLiteral("DEPTH"))
        {
          depthCol = c;
          break;
        }
      }

      QVector<CurveData> curves;
      for (int c = 0; c < header.size(); ++c)
      {
        if (c == 0 || c == depthCol || header.at(c).trimmed() == QStringLiteral("TVD") || header.at(c).trimmed() == QStringLiteral("TVDSS"))
          continue;

        CurveData cd;
        cd.name = header.at(c).trimmed();
        cd.unit = pickUnitForMnemonic(cd.name);
        cd.color = pickWellCurveColor(cd.name, curves.size());
        cd.mode = CurveDisplayMode::Continuous;
        curves.append(cd);
      }

      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() <= depthCol) continue;

        bool ok = false;
        const float d = row.at(depthCol).trimmed().toFloat(&ok);
        if (!ok) continue;

        if (outData.wellName.isEmpty() && !row.isEmpty() && !row.first().isEmpty())
          outData.wellName = row.first().trimmed();

        int cIdx = 0;
        for (int c = 0; c < header.size(); ++c)
        {
          if (c == 0 || c == depthCol || header.at(c).trimmed() == QStringLiteral("TVD") || header.at(c).trimmed() == QStringLiteral("TVDSS"))
            continue;

          float val = std::numeric_limits<float>::quiet_NaN();
          if (c < row.size())
          {
            const QString &cell = row.at(c).trimmed();
            if (!cell.isEmpty())
            {
              bool vok = false;
              const float v = cell.toFloat(&vok);
              if (vok && v > -900.0f)
                val = v;
            }
          }

          if (cIdx < curves.size())
          {
            curves[cIdx].depths.append(d);
            curves[cIdx].values.append(val);
          }
          ++cIdx;
        }
      }

      // 计算各曲线的 minScale / maxScale (过滤 -9999 等无效值)
      for (auto &cd : curves)
      {
        float minV = std::numeric_limits<float>::max();
        float maxV = std::numeric_limits<float>::lowest();
        for (float v : cd.values)
        {
          if (std::isfinite(v) && v > -900.0f)
          {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
          }
        }
        if (minV <= maxV)
        {
          cd.minScale = std::floor(minV);
          cd.maxScale = std::ceil(maxV);
          if (std::abs(cd.maxScale - cd.minScale) < 1e-3f)
            cd.maxScale = cd.minScale + 10.0f;
        }
        outData.continuousCurves.append(cd);
      }
    }
    else if (sheetName.startsWith(QStringLiteral("离散曲线")))
    {
      const QStringList &header = rows.first();
      int depthCol = 1;
      for (int c = 0; c < header.size(); ++c)
      {
        if (header.at(c).trimmed() == QStringLiteral("深度"))
        {
          depthCol = c;
          break;
        }
      }

      QVector<CurveData> curves;
      for (int c = 0; c < header.size(); ++c)
      {
        if (c == 0 || c == depthCol || header.at(c).trimmed() == QStringLiteral("TVD") || header.at(c).trimmed() == QStringLiteral("TVDSS"))
          continue;

        CurveData cd;
        cd.name = header.at(c).trimmed();
        cd.unit = pickUnitForMnemonic(cd.name);
        cd.color = pickWellCurveColor(cd.name, curves.size() + 4);
        cd.mode = CurveDisplayMode::Discrete;
        curves.append(cd);
      }

      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() <= depthCol) continue;
        bool ok = false;
        const float d = row.at(depthCol).trimmed().toFloat(&ok);
        if (!ok) continue;

        int cIdx = 0;
        for (int c = 0; c < header.size(); ++c)
        {
          if (c == 0 || c == depthCol || header.at(c).trimmed() == QStringLiteral("TVD") || header.at(c).trimmed() == QStringLiteral("TVDSS"))
            continue;

          float val = std::numeric_limits<float>::quiet_NaN();
          if (c < row.size())
          {
            const QString &cell = row.at(c).trimmed();
            if (!cell.isEmpty())
            {
              bool vok = false;
              float v = cell.toFloat(&vok);
              if (vok && v > -900.0f)
                val = v;
            }
          }
          if (cIdx < curves.size() && std::isfinite(val))
          {
            curves[cIdx].depths.append(d);
            curves[cIdx].values.append(val);
          }
          ++cIdx;
        }
      }

      for (auto &cd : curves)
      {
        if (cd.depths.isEmpty()) continue;
        float minV = std::numeric_limits<float>::max();
        float maxV = std::numeric_limits<float>::lowest();
        for (float v : cd.values)
        {
          if (std::isfinite(v) && v > -900.0f)
          {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
          }
        }
        if (minV <= maxV)
        {
          cd.minScale = minV;
          cd.maxScale = maxV;
          if (std::abs(cd.maxScale - cd.minScale) < 1e-3f)
            cd.maxScale = cd.minScale + 10.0f;
        }
        outData.discreteCurves.append(cd);
      }
    }
    else if (sheetName.contains(QStringLiteral("岩性道")))
    {
      // ['井号', '道名', '顶深', '顶TVD', '顶TVDSS', '底深', '底TVD', '底TVDSS', '岩性']
      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 9) continue;

        LithologyInterval li;
        li.topDepth = row.at(2).trimmed().toFloat();
        li.bottomDepth = row.at(5).trimmed().toFloat();
        li.lithoName = row.at(8).trimmed();

        if (li.bottomDepth > li.topDepth && !li.lithoName.isEmpty())
          outData.lithologyIntervals.append(li);
      }
    }
    else if (sheetName.contains(QStringLiteral("地层单位道")))
    {
      // ['井号', '道名', '层号', '顶深', '顶TVD', '顶TVDSS', '底深', '底TVD', '底TVDSS', '相类型']
      static const QVector<QColor> formColors = {
          QColor(QStringLiteral("#FFF59D")), QColor(QStringLiteral("#FFE082")),
          QColor(QStringLiteral("#FFCC80")), QColor(QStringLiteral("#FFAB91")),
          QColor(QStringLiteral("#C5E1A5")), QColor(QStringLiteral("#80CBC4")),
          QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 7) continue;

        FormationInterval fi;
        fi.name = row.at(2).trimmed();
        fi.topDepth = row.at(3).trimmed().toFloat();
        fi.bottomDepth = row.at(6).trimmed().toFloat();
        fi.color = formColors.at(outData.formationIntervals.size() % formColors.size());

        if (fi.bottomDepth > fi.topDepth && !fi.name.isEmpty())
          outData.formationIntervals.append(fi);
      }
    }
    else if (sheetName.contains(QStringLiteral("砂层组道")))
    {
      static const QVector<QColor> sandColors = {
          QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFD54F")),
          QColor(QStringLiteral("#FFCA28")), QColor(QStringLiteral("#FFC107"))};

      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 7) continue;

        FormationInterval fi;
        fi.name = row.at(2).trimmed();
        fi.topDepth = row.at(3).trimmed().toFloat();
        fi.bottomDepth = row.at(6).trimmed().toFloat();
        fi.color = sandColors.at(outData.sandIntervals.size() % sandColors.size());

        if (fi.bottomDepth > fi.topDepth && !fi.name.isEmpty())
          outData.sandIntervals.append(fi);
      }
    }
    else if (sheetName.contains(QStringLiteral("文本道")))
    {
      // ['井号', '道名', '层号', '顶深', '顶TVD', '顶TVDSS', '底深', '底TVD', '底TVDSS', '文本']
      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 10) continue;

        TextInterval ti;
        ti.category = row.at(1).trimmed();
        ti.topDepth = row.at(3).trimmed().toFloat();
        ti.bottomDepth = row.at(6).trimmed().toFloat();
        ti.text = row.at(9).trimmed();

        if (!ti.text.isEmpty())
          outData.textIntervals.append(ti);
      }
    }
    else if (sheetName.contains(QStringLiteral("取心数据道")))
    {
      // ['井号', '道名', '顶深', '顶TVD', '顶TVDSS', '底深', '底TVD', '底TVDSS', '筒次', '心长']
      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 10) continue;

        CoreBarrel cb;
        cb.topDepth = row.at(2).trimmed().toFloat();
        cb.bottomDepth = row.at(5).trimmed().toFloat();
        cb.barrelNo = row.at(8).trimmed();
        cb.recoveredLength = row.at(9).trimmed().toFloat();
        cb.cutLength = qMax(0.1f, cb.bottomDepth - cb.topDepth);
        cb.recoveryRate = qBound(0.0f, (cb.recoveredLength / cb.cutLength) * 100.0f, 100.0f);

        if (cb.bottomDepth > cb.topDepth)
          outData.coreBarrels.append(cb);
      }
    }
    else if (sheetName.contains(QStringLiteral("符号道")))
    {
      // ['井号', '道名', '层号', '顶深', '顶TVD', '顶TVDSS', '底深', '底TVD', '底TVDSS', '符号']
      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 10) continue;

        SymbolItem si;
        si.topDepth = row.at(3).trimmed().toFloat();
        si.bottomDepth = row.at(6).trimmed().toFloat();
        si.label = row.at(9).trimmed();

        if (si.label.contains(QStringLiteral("正旋回")))
          si.kind = SymbolKind::PositiveCycle;
        else if (si.label.contains(QStringLiteral("反旋回")))
          si.kind = SymbolKind::NegativeCycle;
        else if (si.label.contains(QStringLiteral("射孔")))
          si.kind = SymbolKind::Perforation;
        else if (si.label.contains(QStringLiteral("油")))
          si.kind = SymbolKind::OilShow;
        else if (si.label.contains(QStringLiteral("气")))
          si.kind = SymbolKind::GasShow;
        else if (si.label.contains(QStringLiteral("水")))
          si.kind = SymbolKind::WaterShow;

        if (si.bottomDepth >= si.topDepth && !si.label.isEmpty())
          outData.symbolItems.append(si);
      }
    }
    else if (sheetName.contains(QStringLiteral("标准层道")))
    {
      // ['井号', '道名', '层名', '深度', 'TVD', 'TVDSS', '文本']
      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 7) continue;

        const double d = row.at(3).trimmed().toDouble();
        const QString txt = row.at(6).trimmed();
        if (d > 0.0 && !txt.isEmpty())
          outData.standardHorizons.append({d, txt});
      }
    }
    else if (sheetName == QStringLiteral("坐标"))
    {
      for (const auto &row : rows)
      {
        if (row.size() >= 2)
        {
          if (row.at(0).trimmed().compare(QStringLiteral("X"), Qt::CaseInsensitive) == 0)
            outData.x = row.at(1).trimmed().toDouble();
          else if (row.at(0).trimmed().compare(QStringLiteral("Y"), Qt::CaseInsensitive) == 0)
            outData.y = row.at(1).trimmed().toDouble();
        }
      }
    }
  };

  // 流式解析 SpreadsheetML XML
  while (!xml.atEnd())
  {
    xml.readNext();
    if (xml.isStartElement())
    {
      const QString tag = xml.name().toString();
      if (tag == QLatin1String("Worksheet"))
      {
        for (const auto &attr : xml.attributes())
          if (attr.name() == QLatin1String("Name"))
            currentSheetName = attr.value().toString();
        sheetRows.clear();
      }
      else if (tag == QLatin1String("Row"))
      {
        QStringList rowCells;
        int colIndex = 0;

        while (!(xml.isEndElement() && xml.name() == QLatin1String("Row")) && !xml.atEnd())
        {
          xml.readNext();
          if (xml.isStartElement() && xml.name() == QLatin1String("Cell"))
          {
            for (const auto &attr : xml.attributes())
            {
              if (attr.name() == QLatin1String("Index"))
              {
                const int explicitIndex = attr.value().toInt() - 1; // 1-indexed to 0-indexed
                while (colIndex < explicitIndex)
                {
                  rowCells.append(QString());
                  ++colIndex;
                }
              }
            }

            QString cellText;
            while (!(xml.isEndElement() && xml.name() == QLatin1String("Cell")) && !xml.atEnd())
            {
              xml.readNext();
              if (xml.isStartElement() && xml.name() == QLatin1String("Data"))
                cellText = xml.readElementText();
            }

            rowCells.append(cellText);
            ++colIndex;
          }
        }
        sheetRows.append(rowCells);
      }
    }
    else if (xml.isEndElement() && xml.name() == QLatin1String("Worksheet"))
    {
      processSheet(currentSheetName, sheetRows);
      sheetRows.clear();
    }
  }

  if (xml.hasError() && xml.error() != QXmlStreamReader::PrematureEndOfDocumentError)
  {
    if (errorMsg) *errorMsg = xml.errorString();
    return false;
  }

  // 计算综合柱状图的总深度跨度 (过滤掉无效/负值深度)
  double minD = std::numeric_limits<double>::max();
  double maxD = 0.0;

  auto updateSpan = [&](double d) {
    if (d >= 0.0 && d < 20000.0)
    {
      if (d < minD) minD = d;
      if (d > maxD) maxD = d;
    }
  };

  for (const auto &c : outData.continuousCurves)
  {
    if (!c.depths.isEmpty())
    {
      updateSpan(c.depths.first());
      updateSpan(c.depths.last());
    }
  }
  for (const auto &c : outData.discreteCurves)
  {
    if (!c.depths.isEmpty())
    {
      updateSpan(c.depths.first());
      updateSpan(c.depths.last());
    }
  }
  for (const auto &fi : outData.formationIntervals)
  {
    updateSpan(fi.topDepth);
    updateSpan(fi.bottomDepth);
  }
  for (const auto &li : outData.lithologyIntervals)
  {
    updateSpan(li.topDepth);
    updateSpan(li.bottomDepth);
  }
  for (const auto &si : outData.symbolItems)
  {
    updateSpan(si.topDepth);
    updateSpan(si.bottomDepth);
  }
  for (const auto &cb : outData.coreBarrels)
  {
    updateSpan(cb.topDepth);
    updateSpan(cb.bottomDepth);
  }

  if (minD <= maxD && minD != std::numeric_limits<double>::max())
  {
    outData.minDepth = minD;
    outData.maxDepth = maxD;
  }
  else
  {
    outData.minDepth = 0.0;
    outData.maxDepth = 3000.0;
  }

  return !outData.isEmpty();
}

} // namespace WellComposite
