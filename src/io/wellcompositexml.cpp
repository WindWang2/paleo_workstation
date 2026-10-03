// 层：数据
#include "wellcompositexml.h"

#include <QFile>
#include <QHash>
#include <QXmlStreamReader>
#include <cmath>
#include <limits>

namespace WellComposite
{

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
        if (outData.wellName.isEmpty() && !row.first().isEmpty())
          outData.wellName = row.first().trimmed();

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
        fi.unitType = row.at(1).trimmed();
        fi.topDepth = row.at(3).trimmed().toFloat();
        fi.bottomDepth = row.at(6).trimmed().toFloat();
        fi.color = formColors.at(outData.formationIntervals.size() % formColors.size());
        if (outData.wellName.isEmpty() && !row.first().isEmpty())
          outData.wellName = row.first().trimmed();

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
      // 道名 = 沉积相/沉积亚相/沉积微相 的行是三级相区间，接入 faciesIntervals
      // （不再混入文本道）；其余行（取样结论等）保持原样。
      struct FaciesRow
      {
        float top = 0.0f;
        float bot = 0.0f;
        QString name;
      };
      QVector<FaciesRow> majorRows, subRows, microRows;

      for (int r = 1; r < rows.size(); ++r)
      {
        const QStringList &row = rows.at(r);
        if (row.size() < 10) continue;

        const QString category = row.at(1).trimmed();
        const float top = row.at(3).trimmed().toFloat();
        const float bot = row.at(6).trimmed().toFloat();
        const QString text = row.at(9).trimmed();
        if (text.isEmpty()) continue;

        if (category == QStringLiteral("沉积相"))
        {
          if (bot > top) majorRows.append({top, bot, text});
          continue;
        }
        if (category == QStringLiteral("沉积亚相"))
        {
          if (bot > top) subRows.append({top, bot, text});
          continue;
        }
        if (category == QStringLiteral("沉积微相"))
        {
          if (bot > top) microRows.append({top, bot, text});
          continue;
        }

        TextInterval ti;
        ti.category = category;
        ti.topDepth = top;
        ti.bottomDepth = bot;
        ti.text = text;
        outData.textIntervals.append(ti);
      }

      // 以最细一级区间为骨架，按中点包含向上补齐 相/亚相
      const QVector<FaciesRow> *base =
          !microRows.isEmpty() ? &microRows : (!subRows.isEmpty() ? &subRows : &majorRows);
      const int baseLevel = !microRows.isEmpty() ? 2 : (!subRows.isEmpty() ? 1 : 0);
      const auto containing = [](const QVector<FaciesRow> &cands, float mid) -> const FaciesRow * {
        const FaciesRow *best = nullptr;
        for (const auto &c : cands)
        {
          if (mid >= c.top && mid <= c.bot &&
              (!best || (c.bot - c.top) < (best->bot - best->top)))
            best = &c;
        }
        return best;
      };
      // 同一相名映射到稳定淡色；层级越深明度略降
      const auto faciesColor = [](const QString &name, int level) {
        if (name.isEmpty()) return QColor(QStringLiteral("#ECEFF1"));
        const int hue = static_cast<int>(qHash(name) % 360);
        return QColor::fromHsl(hue, 96, 218 - level * 10);
      };

      for (const auto &br : *base)
      {
        const float mid = (br.top + br.bot) * 0.5f;
        FaciesInterval fi;
        fi.topDepth = br.top;
        fi.bottomDepth = br.bot;
        if (baseLevel == 0) fi.majorFacies = br.name;
        if (baseLevel == 1) fi.subFacies = br.name;
        if (baseLevel == 2) fi.microFacies = br.name;
        if (fi.majorFacies.isEmpty())
          if (const FaciesRow *m = containing(majorRows, mid)) fi.majorFacies = m->name;
        if (fi.subFacies.isEmpty())
          if (const FaciesRow *m = containing(subRows, mid)) fi.subFacies = m->name;
        if (fi.microFacies.isEmpty())
          if (const FaciesRow *m = containing(microRows, mid)) fi.microFacies = m->name;
        fi.majorColor = faciesColor(fi.majorFacies, 0);
        fi.subColor = faciesColor(fi.subFacies, 1);
        fi.microColor = faciesColor(fi.microFacies, 2);
        outData.faciesIntervals.append(fi);
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
        if (outData.wellName.isEmpty() && !row.first().isEmpty())
          outData.wellName = row.first().trimmed();
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


// ============================================================================
// wave/wellcomposite-deep：D3.3 派生版本写回 + D3.11 审计表 + D6.1/D6.5 表解析
// ============================================================================
static QString xmlEscape(const QString &s)
{
  QString out = s;
  out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
  out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
  out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
  return out;
}

static void writeCell(QString &xml, int col, const QString &value)
{
  xml += QStringLiteral("<Cell><Data ss:Type=\"String\">%1</Data></Cell>")
             .arg(xmlEscape(value));
  Q_UNUSED(col);
}

static void writeCellF(QString &xml, const QString &value)
{
  xml += QStringLiteral("<Cell><Data ss:Type=\"String\">%1</Data></Cell>")
             .arg(xmlEscape(value));
}

QByteArray writeComprehensiveWellXml(const ComprehensiveWellData &data, const QStringList &auditLines)
{
  QString xml;
  xml += QStringLiteral(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<?mso-application progid=\"Excel.Sheet\"?>\n"
      "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n");

  // 坐标表
  xml += QStringLiteral("<Worksheet ss:Name=\"坐标\"><Table>\n<Row>");
  writeCell(xml, 0, QStringLiteral("X"));
  writeCell(xml, 1, QString::number(data.x, 'f', 2));
  xml += QStringLiteral("</Row>\n<Row>");
  writeCell(xml, 0, QStringLiteral("Y"));
  writeCell(xml, 1, QString::number(data.y, 'f', 2));
  xml += QStringLiteral("</Row>\n</Table></Worksheet>\n");

  // 地层单位道（组段分层）
  if (!data.formationIntervals.isEmpty())
  {
    xml += QStringLiteral("<Worksheet ss:Name=\"地层单位道\"><Table>\n");
    xml += QStringLiteral("<Row>");
    writeCell(xml, 0, QStringLiteral("井号"));
    writeCell(xml, 1, QStringLiteral("道名"));
    writeCell(xml, 2, QStringLiteral("层号"));
    writeCell(xml, 3, QStringLiteral("顶深"));
    writeCell(xml, 4, QStringLiteral("顶TVD"));
    writeCell(xml, 5, QStringLiteral("顶TVDSS"));
    writeCell(xml, 6, QStringLiteral("底深"));
    writeCell(xml, 7, QStringLiteral("底TVD"));
    writeCell(xml, 8, QStringLiteral("底TVDSS"));
    writeCell(xml, 9, QStringLiteral("相类型"));
    xml += QStringLiteral("</Row>\n");
    for (const auto &fi : data.formationIntervals)
    {
      xml += QStringLiteral("<Row>");
      writeCell(xml, 0, data.wellName);
      writeCell(xml, 1, fi.unitType.isEmpty() ? QStringLiteral("地层单位") : fi.unitType);
      writeCell(xml, 2, fi.name);
      writeCellF(xml, QString::number(fi.topDepth, 'f', 2));
      writeCell(xml, 3, QString());
      writeCell(xml, 4, QString());
      writeCellF(xml, QString::number(fi.bottomDepth, 'f', 2));
      writeCell(xml, 5, QString());
      writeCell(xml, 6, QString());
      writeCell(xml, 7, fi.code);
      xml += QStringLiteral("</Row>\n");
    }
    xml += QStringLiteral("</Table></Worksheet>\n");
  }

  // 岩性道
  if (!data.lithologyIntervals.isEmpty())
  {
    xml += QStringLiteral("<Worksheet ss:Name=\"岩性道\"><Table>\n");
    xml += QStringLiteral("<Row>");
    writeCell(xml, 0, QStringLiteral("井号"));
    writeCell(xml, 1, QStringLiteral("道名"));
    writeCell(xml, 2, QStringLiteral("顶深"));
    writeCell(xml, 3, QString());
    writeCell(xml, 4, QString());
    writeCell(xml, 5, QStringLiteral("底深"));
    writeCell(xml, 6, QString());
    writeCell(xml, 7, QString());
    writeCell(xml, 8, QStringLiteral("岩性"));
    xml += QStringLiteral("</Row>\n");
    for (const auto &li : data.lithologyIntervals)
    {
      xml += QStringLiteral("<Row>");
      writeCell(xml, 0, data.wellName);
      writeCell(xml, 1, QStringLiteral("岩性"));
      writeCellF(xml, QString::number(li.topDepth, 'f', 2));
      writeCell(xml, 3, QString());
      writeCell(xml, 4, QString());
      writeCellF(xml, QString::number(li.bottomDepth, 'f', 2));
      writeCell(xml, 6, QString());
      writeCell(xml, 7, QString());
      writeCell(xml, 8, li.lithoName);
      xml += QStringLiteral("</Row>\n");
    }
    xml += QStringLiteral("</Table></Worksheet>\n");
  }

  // 标准层道（标志层/TOPs——D3.1/D3.2 编辑的主要写回对象）
  if (!data.standardHorizons.isEmpty())
  {
    xml += QStringLiteral("<Worksheet ss:Name=\"标准层道\"><Table>\n");
    xml += QStringLiteral("<Row>");
    writeCell(xml, 0, QStringLiteral("井号"));
    writeCell(xml, 1, QStringLiteral("道名"));
    writeCell(xml, 2, QStringLiteral("层名"));
    writeCell(xml, 3, QStringLiteral("深度"));
    writeCell(xml, 4, QStringLiteral("TVD"));
    writeCell(xml, 5, QStringLiteral("TVDSS"));
    writeCell(xml, 6, QStringLiteral("文本"));
    xml += QStringLiteral("</Row>\n");
    for (const auto &m : data.standardHorizons)
    {
      xml += QStringLiteral("<Row>");
      writeCell(xml, 0, data.wellName);
      writeCell(xml, 1, QStringLiteral("标志层"));
      writeCell(xml, 2, m.second);
      writeCellF(xml, QString::number(m.first, 'f', 2));
      writeCell(xml, 4, QString());
      writeCell(xml, 5, QString());
      writeCell(xml, 6, m.second);
      xml += QStringLiteral("</Row>\n");
    }
    xml += QStringLiteral("</Table></Worksheet>\n");
  }

  // 文本道（含沉积相三级行，读回时可再分流）
  const auto writeTextSheet = [&xml](const QString &sheetName, const QString &category,
                                     const auto &rows) {
    xml += QStringLiteral("<Worksheet ss:Name=\"%1\"><Table>\n").arg(sheetName);
    xml += QStringLiteral("<Row>");
    writeCell(xml, 0, QStringLiteral("井号"));
    writeCell(xml, 1, QStringLiteral("道名"));
    writeCell(xml, 2, QStringLiteral("层号"));
    writeCell(xml, 3, QStringLiteral("顶深"));
    writeCell(xml, 4, QString());
    writeCell(xml, 5, QString());
    writeCell(xml, 6, QStringLiteral("底深"));
    writeCell(xml, 7, QString());
    writeCell(xml, 8, QString());
    writeCell(xml, 9, QStringLiteral("文本"));
    xml += QStringLiteral("</Row>\n");
    for (const auto &r : rows)
    {
      xml += QStringLiteral("<Row>");
      writeCell(xml, 0, r.well);
      writeCell(xml, 1, category);
      writeCell(xml, 2, QString());
      writeCellF(xml, QString::number(r.top, 'f', 2));
      writeCell(xml, 4, QString());
      writeCell(xml, 5, QString());
      writeCellF(xml, QString::number(r.bottom, 'f', 2));
      writeCell(xml, 7, QString());
      writeCell(xml, 8, QString());
      writeCell(xml, 9, r.text);
      xml += QStringLiteral("</Row>\n");
    }
    xml += QStringLiteral("</Table></Worksheet>\n");
  };

  if (!data.textIntervals.isEmpty())
  {
    struct Row
    {
      QString well;
      double top = 0, bottom = 0;
      QString text;
    };
    QVector<Row> rows;
    for (const auto &ti : data.textIntervals)
      rows << Row{data.wellName, ti.topDepth, ti.bottomDepth,
                  ti.category.isEmpty() ? ti.text : ti.text};
    writeTextSheet(QStringLiteral("文本道"), QStringLiteral("取样结论"), rows);
  }

  if (!data.faciesIntervals.isEmpty())
  {
    struct Row
    {
      QString well;
      double top = 0, bottom = 0;
      QString text;
    };
    QVector<Row> rows;
    for (const auto &fi : data.faciesIntervals)
    {
      rows << Row{data.wellName, fi.topDepth, fi.bottomDepth, fi.majorFacies};
      rows << Row{data.wellName, fi.topDepth, fi.bottomDepth, fi.subFacies};
      rows << Row{data.wellName, fi.topDepth, fi.bottomDepth, fi.microFacies};
    }
    writeTextSheet(QStringLiteral("文本道-相"), QStringLiteral("沉积微相"), rows);
  }

  // D3.11 编辑审计工作表（manifest 风格操作历史）
  if (!auditLines.isEmpty())
  {
    xml += QStringLiteral("<Worksheet ss:Name=\"编辑审计\"><Table>\n");
    xml += QStringLiteral("<Row>");
    writeCell(xml, 0, QStringLiteral("时间"));
    writeCell(xml, 1, QStringLiteral("操作"));
    writeCell(xml, 2, QStringLiteral("细节"));
    xml += QStringLiteral("</Row>\n");
    for (const QString &line : auditLines)
    {
      const QStringList parts = line.split(QStringLiteral(" | "));
      xml += QStringLiteral("<Row>");
      writeCell(xml, 0, parts.value(0));
      writeCell(xml, 1, parts.value(1));
      writeCell(xml, 2, parts.value(2));
      xml += QStringLiteral("</Row>\n");
    }
    xml += QStringLiteral("</Table></Worksheet>\n");
  }

  xml += QStringLiteral("</Workbook>\n");
  return xml.toUtf8();
}

bool writeComprehensiveWellXmlFile(const ComprehensiveWellData &data, const QString &filePath,
                                   const QStringList &auditLines, QString *errorMsg)
{
  QFile f(filePath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (errorMsg)
      *errorMsg = QStringLiteral("无法写入文件: %1").arg(filePath);
    return false;
  }
  f.write(writeComprehensiveWellXml(data, auditLines));
  return true;
}

// D6.1/D6.5：从 XML 抽取指定工作表的行（复用流式行收集器）
namespace {
bool collectSheetRows(const QString &filePath, const QString &sheetNamePrefix,
                      QVector<QStringList> &rowsOut, QString *errorMsg)
{
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly))
  {
    if (errorMsg)
      *errorMsg = QStringLiteral("无法打开文件: %1").arg(filePath);
    return false;
  }

  QXmlStreamReader xml(file.readAll());
  QString currentSheetName;
  QVector<QStringList> sheetRows;
  bool found = false;

  while (!xml.atEnd())
  {
    xml.readNext();
    if (xml.isStartElement())
    {
      const QString tag = xml.name().toString();
      if (tag == QLatin1String("Worksheet"))
      {
        currentSheetName.clear();
        for (const auto &attr : xml.attributes())
          if (attr.name() == QLatin1String("Name"))
            currentSheetName = attr.value().toString();
        sheetRows.clear();
      }
      else if (tag == QLatin1String("Row") && currentSheetName.startsWith(sheetNamePrefix))
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
                const int explicitIndex = attr.value().toInt() - 1;
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
      if (currentSheetName.startsWith(sheetNamePrefix))
      {
        rowsOut = sheetRows;
        found = true;
      }
      sheetRows.clear();
    }
  }
  return found;
}
} // namespace

bool parseDeviationSurvey(const QString &filePath, QVector<XmlDeviationStation> &out, QString *errorMsg)
{
  out.clear();
  QVector<QStringList> rows;
  if (!collectSheetRows(filePath, QStringLiteral("井斜"), rows, errorMsg))
    return false;

  // 列：测深/MD | 井斜角 | 方位角（表头行自动跳过）
  for (const auto &row : rows)
  {
    if (row.size() < 3)
      continue;
    bool ok1 = false, ok2 = false, ok3 = false;
    const double md = row.at(0).trimmed().toDouble(&ok1);
    const double inc = row.at(1).trimmed().toDouble(&ok2);
    const double azi = row.at(2).trimmed().toDouble(&ok3);
    if (!ok1 || !ok2 || !ok3)
      continue; // 表头/坏行
    if (md < 0.0)
      continue;
    out.append({md, inc, azi});
  }
  return !out.isEmpty();
}

bool parseTimeDepthTable(const QString &filePath, QVector<XmlTimeDepthPair> &out, QString *errorMsg)
{
  out.clear();
  QVector<QStringList> rows;
  if (!collectSheetRows(filePath, QStringLiteral("时深"), rows, errorMsg))
    return false;

  for (const auto &row : rows)
  {
    if (row.size() < 2)
      continue;
    bool ok1 = false, ok2 = false;
    const double tvd = row.at(0).trimmed().toDouble(&ok1);
    const double twt = row.at(1).trimmed().toDouble(&ok2);
    if (!ok1 || !ok2 || tvd < 0.0)
      continue;
    out.append({tvd, twt});
  }
  return !out.isEmpty();
}

} // namespace WellComposite
