// 层：功能
#include "wellsectionexport.h"

#include "io/atomicwrite.h"

namespace wellsection {

bool exportTopsCsv(const QString &path, const QString &csvText, QString *error)
{
  // 旧实现是 ui 层 QFile + Truncate 原地覆写：写中途失败/磁盘满会把目标
  // 截断，而同名的上一份完整 CSV 已被抹掉——导出失败反而破坏旧文件。
  return paleo::writeFileAtomic(path, csvText.toUtf8(), error);
}

} // namespace wellsection
