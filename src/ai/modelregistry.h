// 层：功能
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// ai/ — 模型注册表（goal/ai-geological-assist 范围5）。
// 「models/ 目录 + manifest.json」约定（appcontext 已按 <工程>/models 钉模型根）：
//   { "models": [ {
//       "name": "seg3",            // 展示名（loadModel 参数）
//       "file": "seg3.onnx",       // 相对 models/ 的文件名
//       "version": "1.0.0",
//       "task": "segmentation",    // segmentation | trace_scorer | …
//       "dataType": "amplitude",   // 适用数据类型（振幅/属性）
//       "input":  { "name": "x", "dtype": "float32", "shape": [1,1,null,null] },
//       "outputs": { "semantics": "logits", "classes": 3 },
//       "sha256": "…"              // 可选钉哈希；不符 → FingerprintMismatch
//   } ] }
// 扫描语义（如实降级，不报错轰炸）：
//   · manifest 不存在 / models 空 → 「未装模型」状态（manifestFound=false，
//     manifestError 空）——面板显示提示而非错误。
//   · manifest 存在但坏 JSON → manifestError 如实记录。
//   · 条目缺必填（name/file）→ 该条 ManifestInvalid，其余条目不受牵连。
//   · 文件缺失 → FileMissing；钉哈希不符 → FingerprintMismatch。
//   · #145 路径约束：name 必须是单个路径段；file 必须是相对路径、不含 ..
//     段、canonical 后仍位于 models/ 内（防符号链接逃逸）→ 否则 ManifestInvalid。
//   · 重名 name：后出现的条目 ManifestInvalid（不静默覆盖）。

struct ModelRegistryEntry
{
  enum class Status
  {
    Ok,
    FileMissing,
    ManifestInvalid,
    FingerprintMismatch,
    Unreadable,
  };
  Status status = Status::Ok;
  QString detail; // 诚实错误链（非 Ok 时必非空）

  // manifest 字段
  QString name;
  QString file;
  QString version;
  QString task;
  QString dataType;
  QString inputName;
  QString inputDtype;
  QVector<int64_t> inputShape; // null → -1（动态维）
  QString outputSemantics;
  int classes = 0;
  QString sha256Pinned;

  // 扫描期实测
  QString sha256Actual;
  QString absolutePath;
};

struct ModelRegistryScan
{
  bool manifestFound = false;
  QString manifestError; // manifest 在但解析失败（坏 JSON/根类型不符）
  QList<ModelRegistryEntry> entries;

  bool anyRunnable() const;
  QStringList runnableNames() const; // status==Ok 的 name（供算法下拉）
};

class ModelRegistry
{
  public:
    static ModelRegistryScan scan( const QString &modelsDir );
    static QString statusLabel( ModelRegistryEntry::Status s );
    // 单段文件名：非空、无 / \\、不是 . / ..、非绝对路径。
    static bool isSafeModelName( const QString &name );
    // file 相对 modelsDir 且不出目录（词法检查；canonical 检查在 scan 内）。
    static bool isSafeRelativeFile( const QString &file );
};
