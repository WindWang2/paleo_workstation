// 层：功能
#pragma once
#include <QString>

// workflow/wellsectionexport — 连井剖面导出产物的落盘编排（#729）。
//
// 契约：原子整写。ui 层 io/* 白名单仅 lasdoc.h——视图不直接 include
// io/atomicwrite.h，写点收在功能层（同 mapbookqueue 的 manifest 口径）：
// 旁写临时件 → 校验 write()==size → commit() 整体替换。任一步失败目标路径
// 保持原样（同名旧文件完好或不存在），不留截断/半截文件，也没有「先删旧件
// 再改名」的丢档窗口。视图只传路径与已渲染文本，不碰 QFile/QSaveFile。
namespace wellsection
{

// 层位井深表 CSV 落位（UTF-8 字节原样写）。error 可空；失败返回 false 并
// 写 *error（QSaveFile 的 errorString + 路径）。
bool exportTopsCsv(const QString &path, const QString &csvText,
                   QString *error = nullptr);

} // namespace wellsection
