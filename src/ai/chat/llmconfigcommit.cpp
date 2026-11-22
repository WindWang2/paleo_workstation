// 层：功能
#include "llmconfigcommit.h"
#include "llmkeystore.h"

#include <QFile>
#include <QSaveFile>

namespace {
// 结构校验（地址/模型）：密钥单查——密钥缺失允许保存（助手保持禁用态，
// 状态行如实说明），地址/模型非法则不落盘。
QString structuralProblem(const LlmConfig &form) {
  LlmConfig probe = form;
  probe.apiKey = QByteArrayLiteral("probe-placeholder");
  return probe.validate();
}
} // namespace

LlmConfigCommitHooks defaultLlmConfigCommitHooks() {
  LlmConfigCommitHooks hooks;
  hooks.readFile = [] {
    QFile file(LlmConfig::path());
    if (!file.open(QIODevice::ReadOnly))
      return QByteArray();
    return file.readAll();
  };
  hooks.writeConfig = [](const LlmConfig &form, QString *error) {
    return form.save(error); // 原子（QSaveFile）+ 形状单一来源（密钥不落盘）
  };
  hooks.writeFileBytes = [](const QByteArray &bytes, QString *error) {
    QSaveFile file(LlmConfig::path());
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
      if (error)
        *error = file.errorString();
      return false;
    }
    return true;
  };
  hooks.writeKey = [](QObject *context, const QByteArray &key,
                      std::function<void(bool, const QString &)> done) {
    LlmKeyStore::write(context, key, std::move(done));
  };
  hooks.clearKey = [](QObject *context,
                      std::function<void(bool, const QString &)> done) {
    LlmKeyStore::clear(context, std::move(done));
  };
  return hooks;
}

LlmConfigCommitter::LlmConfigCommitter(QObject *parent) : QObject(parent) {
  m_hooks = defaultLlmConfigCommitHooks();
}

LlmConfigCommitter::~LlmConfigCommitter() = default;

void LlmConfigCommitter::submit(
  const LlmConfig &form, const QByteArray &newKey, bool clearKey,
  const LlmConfig &current, QObject *callbackContext,
  std::function<void(bool ok, const QString &error)> done,
  std::function<void(const LlmConfig &applied)> apply) {
  Pending pending;
  pending.form = form;
  pending.newKey = newKey;
  pending.clearKey = clearKey;
  pending.current = current;
  pending.callbackContext = callbackContext;
  pending.done = std::move(done);
  pending.apply = std::move(apply);
  m_queue.append(std::move(pending));
  pump();
}

void LlmConfigCommitter::pump() {
  if (m_inFlight || m_queue.isEmpty())
    return;
  runHead();
}

void LlmConfigCommitter::runHead() {
  const Pending head = m_queue.first(); // 异步段只经拷贝（出队后引用失效）
  const QString structural = structuralProblem(head.form);
  if (!structural.isEmpty()) {
    completeHead(false, structural, head, nullptr);
    return;
  }
  // ① 快照旧文件原文（补偿基准；此前未配置 → 空串）。
  const QByteArray previousBytes = m_hooks.readFile();
  // ② 写新配置文件（原子、不含密钥）。失败：钥匙串没动过，两存储仍一致，
  // 如实报错即可（QSaveFile 失败不产生半写文件）。
  QString error;
  if (!m_hooks.writeConfig(head.form, &error)) {
    completeHead(false, QObject::tr("配置写入失败：%1").arg(error), head,
                 nullptr);
    return;
  }
  // ③ 密钥操作（串行链）：newKey 优先（clear 冗余忽略）；clearKey 且无新
  // 密钥 = 清除；都无 = 沿用（不动存储，直接生效）。
  if (!head.newKey.isEmpty()) {
    m_inFlight = true;
    m_hooks.writeKey(
      this, head.newKey,
      [this, previousBytes, head](bool ok, const QString &err) {
        onKeyOpFinished(previousBytes, head, ok, err, /*wasClear=*/false);
      });
    return;
  }
  if (head.clearKey) {
    m_inFlight = true;
    m_hooks.clearKey(
      this, [this, previousBytes, head](bool ok, const QString &err) {
        onKeyOpFinished(previousBytes, head, ok, err, /*wasClear=*/true);
      });
    return;
  }
  LlmConfig applied = head.form;
  applied.apiKey = head.current.apiKey; // 密钥维持现状
  completeHead(true, QString(), head, &applied);
}

void LlmConfigCommitter::onKeyOpFinished(const QByteArray &previousBytes,
                                         Pending head, bool ok,
                                         const QString &keyError,
                                         bool wasClear) {
  if (m_queue.isEmpty())
    return; // 防御：链已被清理（本对象在途销毁的兜底路径）
  m_inFlight = false;
  if (!ok) {
    const QString cause =
      wasClear ? QObject::tr("清除密钥失败：%1").arg(keyError)
               : QObject::tr("密钥写入钥匙串失败：%1").arg(keyError);
    compensateAndComplete(previousBytes, cause, std::move(head));
    return;
  }
  LlmConfig applied = head.form;
  applied.apiKey = wasClear ? QByteArray() : head.newKey;
  completeHead(true, QString(), std::move(head), &applied);
}

void LlmConfigCommitter::compensateAndComplete(
  const QByteArray &previousBytes, const QString &cause, Pending head) {
  // 密钥操作失败时文件已写新值——还原旧原文，两存储回到旧的一致组合。
  QString detail = cause;
  if (!previousBytes.isEmpty()) {
    QString restoreError;
    if (m_hooks.writeFileBytes(previousBytes, &restoreError)) {
      completeHead(false, detail, std::move(head), nullptr);
      return;
    }
    detail += QObject::tr("；配置文件还原失败：%1").arg(restoreError);
  }
  // 无旧文件可还原（此前未配置，钥匙串仍可能残留旧密钥），或还原失败：
  // 降级清钥匙串——「新端点文件 + 旧密钥」的未验证组合绝不能留给重启自动
  // 启用；清掉密钥后重启是诚实禁用态，须重新配置。
  m_inFlight = true;
  m_hooks.clearKey(this, [this, detail, head](bool ok, const QString &err) {
    m_inFlight = false;
    QString finalError = detail;
    if (!ok)
      finalError += QObject::tr("；钥匙串密钥清理也失败：%1（须手动重新配置）")
                        .arg(err);
    completeHead(false, finalError, std::move(head), nullptr);
  });
}

void LlmConfigCommitter::completeHead(bool ok, const QString &error,
                                      Pending head, const LlmConfig *applied) {
  // 出队：串行链保证本提交即队头（同步路径由 pump 直达，异步路径在
  // m_inFlight 闸后排空后才回调）。apply 先于 done（契约：done(ok) 时配置
  // 已生效）；apply 无条件执行（协议链不依赖 UI 生命周期），done 只在
  // callbackContext 存活时触发。回调里再 submit 的新提交由尾部 pump 接链。
  if (!m_queue.isEmpty())
    m_queue.removeFirst();
  if (ok && applied)
    head.apply(*applied);
  if (head.done && head.callbackContext)
    head.done(ok, error);
  pump();
}
