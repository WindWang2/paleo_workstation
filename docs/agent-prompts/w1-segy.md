你在 paleo_workstation 项目开发 SEG-Y 地震数据读取与真实剖面渲染。

【执行方式】本任务必须使用 teamwork-preview 模式执行：自行拆解子任务
（解析器核心 / 渲染接线 / 测试与验证），能并行的部分用 teamwork-preview
并行开 worker，不能并行的串行。每个子任务完成后在 teamwork 内汇总状态。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui），
moc=/usr/lib/qt6/moc，测试用 QT_QPA_PLATFORM=offscreen 的 QTest。
系统 QGIS 已装，勿 vendor。

【第一步：建 worktree（ teamwork-preview 的 lead 先做，子任务共享此 worktree ）】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-segy -b wave/segy
cd ../pw-segy   ← 之后所有工作在此 worktree 内进行

【任务背景】src/ui/seismicpreviewpanel.{h,cpp} 现有伪随机 profile 占位渲染；
src/io/lasparser.{h,cpp} 是文本解析器的风格参考（纯 Qt，无 Qgs）。

【交付物】
1. src/io/segyreader.h/.cpp — SEG-Y rev0/1 最小解析器（纯 Qt+QFile/QByteArray，
   不引 Qgs*）：读 binary header（400字节，大端），识别样本格式（ibm fp32 /
   ieee fp32），逐道读 240 字节道头+样本数组。暴露：
     struct SegyTrace { qint32 cdp; qint32 lineNo; QVector<float> samples; qint64 tracl; };
     class SegyReader {
       static bool open(const QString &path, QString *error=nullptr);
       int traceCount() const; int samplesPerTrace() const; float sampleIntervalUs() const;
       QVector<SegyTrace> traces() const;
     };
   截断/格式错误/空文件 → false+error。CDP 从道头 bytes 20-23 (big endian int32)。
2. src/ui/seismicpreviewpanel.cpp — 新增 void loadLineFromFile(const QString &assetId,
   const QString &segyPath)：用 SegyReader 读道，把 scene 里的伪 trace 换成真实
   变密度渲染（每道一列像素，样本值→灰度；或波形线，二选一，注释说明取舍）；
   保持 updatePreview(assetId)/empty state/选择联动语义不动。
3. tests/tst_segy.cpp — 在测试内合成一个 ~3 道 × 64 样本的最小合法 SEG-Y 文件
   （QByteArray 手写头），覆盖：头解析字段、traces() 数量与样本值、
   截断文件报错、非 SEG-Y 文件报错、panel loadLineFromFile 后 scene 有真内容。

【纪律】TDD：先写测试（红）再实现（绿）。standalone /tmp 编译验证，不跑主构建。
不碰：CMakeLists.txt、paleomainwindow.*、pagepanels.*、workflows.*（父层收尾接线）。
完成后在 worktree 分支 git add -A && git commit，提交信息遵循仓库惯例
（含 Generated with [Devin] trailer）。报告：测试通过数、改动文件清单、
给父层（集成者）的注意事项。
