// 层：数据
#include "horizonnativegrid.h"
#include "horizonbinner.h"
#include <QTextStream>
#include <cmath>
#include <algorithm>

namespace {
bool solve(double m[3][3], double b[3], double out[3])
{
  for (int i = 0; i < 3; ++i) {
    int pivot = i;
    for (int j = i + 1; j < 3; ++j) if (std::abs(m[j][i]) > std::abs(m[pivot][i])) pivot = j;
    if (std::abs(m[pivot][i]) < 1e-10) return false;
    for (int j = 0; j < 3; ++j) std::swap(m[i][j], m[pivot][j]); std::swap(b[i], b[pivot]);
    const double scale = m[i][i];
    for (int j = 0; j < 3; ++j) m[i][j] /= scale; b[i] /= scale;
    for (int k = 0; k < 3; ++k) if (k != i) {
      const double f = m[k][i]; for (int j = 0; j < 3; ++j) m[k][j] -= f * m[i][j]; b[k] -= f * b[i];
    }
  }
  for (int i = 0; i < 3; ++i) out[i] = b[i];
  return true;
}
struct Point { double col, row, x, y; };
}
bool nativeHorizonGrid(const QByteArray &text, const HorizonHeader &h, BinnedHorizon *grid, QString *error)
{
  // Legacy local-grid files keep their declared header contract. Explicit
  // native projection files may have stale copied P1/P2/P3 coordinates.
  const auto header = text.left(8192).toUpper();
  if (!header.contains("# PROJECTION: UTM") && !header.contains("# PROJECTION: EPSG:")) return true;
  QVector<Point> points;
  QTextStream stream(text);
  double normal[3][3]{}, bx[3]{}, by[3]{};
  while (!stream.atEnd()) {
    const auto line = stream.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#')) continue;
    const auto fields = line.simplified().split(' ');
    if (fields.size() < 5) continue;
    bool okX, okY, okI, okC;
    const double x = fields[0].toDouble(&okX), y = fields[1].toDouble(&okY);
    const qint64 inl = fields[3].toLongLong(&okI), cross = fields[4].toLongLong(&okC);
    const double row = static_cast<double>(h.gridRows - 1) - static_cast<double>(inl - h.p1Inline), col = static_cast<double>(cross - h.p1Xline);
    if (!okX || !okY || !okI || !okC || !std::isfinite(x) || !std::isfinite(y) || row < 0 || col < 0 || row >= h.gridRows || col >= h.gridCols) continue;
    const double v[3]{1, col, row};
    for (int i = 0; i < 3; ++i) { bx[i] += v[i] * x; by[i] += v[i] * y; for (int j = 0; j < 3; ++j) normal[i][j] += v[i] * v[j]; }
    points << Point{col, row, x, y};
  }
  double mx[3][3], my[3][3], ax[3], ay[3];
  for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) mx[i][j] = my[i][j] = normal[i][j];
  if (!solve(mx, bx, ax) || !solve(my, by, ay)) { if (error) *error = QStringLiteral("投影层位缺少足够的有效 XY 与线道号，无法定位网格"); return false; }
  double residual = 0;
  for (const auto &p : points) residual = std::max(residual, std::hypot(ax[0] + ax[1]*p.col + ax[2]*p.row - p.x, ay[0] + ay[1]*p.col + ay[2]*p.row - p.y));
  const double cell = std::min(std::hypot(ax[1], ay[1]), std::hypot(ax[2], ay[2]));
  if (!(cell > 0) || std::abs(ax[1]*ay[2] - ax[2]*ay[1]) < cell * cell * 1e-9 || residual > cell * 0.5) { if (error) *error = QStringLiteral("层位 XY 无法表示为规则网格（最大误差 %1 m），请先重采样后导入").arg(residual); return false; }
  grid->originX = ax[0]; grid->originY = ay[0]; grid->dx = ax[1]; grid->dy = -ay[2];
  grid->rowX = ax[2]; grid->columnY = ay[1]; grid->geometryResidualM = residual;
  return true;
}
