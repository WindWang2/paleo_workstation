// 层：视图
#include "datalist.h"
#include <QVBoxLayout>
void DataListPanel::buildImportButtons(QVBoxLayout *importLay, QWidget *importSection) {
  const struct { const char *name; const char *text; const char *kind; const char *desc; }
      kImports[] = {
    {"importPaleomap", QT_TR_NOOP("导入现有古地理图"), "paleomap", QT_TR_NOOP("按层位管理古地理图，Shapefile 配套文件作为一项")},
    {"importPaleomapFolder", QT_TR_NOOP("导入古地理图文件夹"), "paleomap_folder", QT_TR_NOOP("递归导入并识别图件层位")},
    {"importFiles", QT_TR_NOOP("导入文件"), "files", QT_TR_NOOP("选择一个或多个资料文件导入")},
    {"importWells", QT_TR_NOOP("导入井数据"), "wells",
     QT_TR_NOOP("选择一个或多个井位/分层文件入库")},
    {"importWellLogs", QT_TR_NOOP("导入测井数据"), "well_log",
     QT_TR_NOOP("选择 LAS 测井曲线或 XML/Excel 综合柱状图入库，可多选")},
    {"importSeismic", QT_TR_NOOP("导入地震数据"), "seismic",
     QT_TR_NOOP("选择 SEG-Y 等地震数据文件入库，可多选")},
    {"importBoundary", QT_TR_NOOP("导入边界数据"), "boundary",
     QT_TR_NOOP("选择边界矢量文件入库，可多选")},
    {"importFolder", QT_TR_NOOP("导入工区文件夹"), "folder",
     QT_TR_NOOP("选择工区目录：确认每个文件的类型后整目录入库；"
                "识别不了的类型按参考资料/未归类入库")},
  };
  for (const auto &spec : kImports)
  {
    auto *btn = new QPushButton(tr(spec.text), importSection);
    btn->setObjectName(QLatin1String(spec.name));
    // T32 a11y：导入入口各自报名（屏幕阅读器不读图标猜测）。
    btn->setAccessibleName(tr(spec.text));
    btn->setAccessibleDescription(tr(spec.desc));
    btn->setToolTip(tr(spec.desc));
    connect(btn, &QPushButton::clicked, this,
            [this, kind = QLatin1String(spec.kind)] { emit importRequested(kind); });
    importLay->addWidget(btn);
  }
}
