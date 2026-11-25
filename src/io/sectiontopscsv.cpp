// 层：数据
#include "sectiontopscsv.h"
#include <cmath>

namespace paleo::io {
namespace {
// Quoted names in the existing section CSV may contain commas, quotes or complete lines.
bool readRecord(const QString &text, qsizetype &pos, QStringList &record) {
    while (pos < text.size()) {
        QString value;
        if (text[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < text.size()) {
                const QChar c = text[pos++];
                if (c != '"') { value += c; continue; }
                if (pos < text.size() && text[pos] == '"') { value += c; ++pos; continue; }
                closed = true;
                break;
            }
            if (!closed) return false;
        } else {
            const auto begin = pos;
            while (pos < text.size() && text[pos] != ',' && text[pos] != '\r' && text[pos] != '\n') {
                if (text[pos] == '"') return false;
                ++pos;
            }
            value = text.sliced(begin, pos - begin);
        }
        record.append(value);
        if (pos == text.size()) return true;
        const auto separator = text[pos++];
        if (separator == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
        if (separator == '\r' || separator == '\n') return true;
        if (separator != ',') return false;
        if (pos == text.size()) record.append(QString());
    }
    return true;
}
}

WorkbookReadResult readSectionTopsCsv(const QString &text, int rowLimit, bool *limited) {
    WorkbookReadResult out;
    out.format = QLatin1String(sectionTopsProfile);
    WorkbookSheet sheet;
    qsizetype pos = 0;
    if (!readRecord(text, pos, sheet.headers) ||
        (sheet.headers.size() != 5 && sheet.headers.size() != 6) ||
        sheet.headers.value(2) != QStringLiteral("MD(m)") ||
        (sheet.headers.size() == 6 && sheet.headers.value(3) != QStringLiteral("TVD(m)")) ||
        sheet.headers[sheet.headers.size() - 2] != QStringLiteral("基准面")) {
        out.error = QStringLiteral("井表分层交换列须声明 MD(m) 和可选的 TVD(m)，请核对深度类型与单位");
        return out;
    }
    while (pos < text.size()) {
        if (rowLimit >= 0 && sheet.rows.size() >= rowLimit) { if (limited) *limited = true; break; }
        QStringList row;
        if (!readRecord(text, pos, row)) {
            if (limited && *limited && pos == text.size()) break; // bounded preview stopped inside a quoted field
            out.error = QStringLiteral("井表分层 CSV 引号或记录未闭合"); return out;
        }
        if (row == QStringList{QString()}) continue;
        sheet.rows.append(row);
    }
    out.sheets.append(sheet);
    out.ok = true;
    return out;
}

WellTable sectionTopsTable(const WorkbookReadResult &workbook) {
    WellTable out;
    out.type = QStringLiteral("well_stratification");
    out.mapping.insert("profile", QLatin1String(sectionTopsProfile));
    out.error = workbook.error;
    if (!out.error.isEmpty() || workbook.sheets.isEmpty()) return out;
    const auto &sheet = workbook.sheets.first();
    out.columns = sheet.headers;
    const bool tvdColumn = out.columns.size() == 6;
    int record = 1;
    for (const auto &row : sheet.rows) {
        ++record;
        WellTopRecord pick;
        pick.wellName = row.value(0); pick.topName = row.value(1);
        pick.md = row.value(2).toDouble(&pick.hasMd);
        const auto tvd = row.value(3);
        if (tvdColumn && !tvd.isEmpty()) pick.tvd = tvd.toDouble(&pick.hasTvd);
        if (row.size() != (tvdColumn ? 4 : 3) || pick.wellName.trimmed().isEmpty() ||
            pick.topName.trimmed().isEmpty() || !pick.hasMd || !std::isfinite(pick.md) ||
            (tvdColumn && !tvd.isEmpty() && (!pick.hasTvd || !std::isfinite(pick.tvd)))) {
            out.error = QStringLiteral("井表分层 CSV 第 %1 条记录的井名、顶名或深度无效，请核对").arg(record);
            out.tops.clear();
            return out;
        }
        if (out.preview.size() < 8) out.preview.append(row);
        out.tops.append(pick);
        ++out.report.acceptedRows;
    }
    // Flatten/elevation options label the drawing, not a measured-depth reference.
    out.report.issues.append(QStringLiteral("深度单位为 m，物理起算面未声明，基准面列仅描述剖面显示"));
    return out;
}
}
