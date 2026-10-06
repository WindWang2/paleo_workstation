// 层：功能
#include "faciesclassify.h"
#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/qgislayerservice.h"
#include "services/faciesclassificationservice.h"
#include "services/faciestraining.h"
#include "workflow/derivedassets.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
namespace paleo::crossplot {
FaciesClassifyWorkflow::FaciesClassifyWorkflow(PaleoTaskService *t,
                                               PaleoProjectStore *s,
                                               QgisLayerService *l, QObject *p)
    : QObject(p), m_tasks(t), m_store(s), m_layers(l) {}
void FaciesClassifyWorkflow::setCatalog(DataCatalog *c, const QString &dir) {
  clear();
  m_catalog = c;
  m_projectDir = dir;
}
void FaciesClassifyWorkflow::clear() {
  cancel();
  ++m_generation;
  m_task = nullptr;
  emit busyChanged(false);
  m_samples.reset();
  m_classification = {};
}
void FaciesClassifyWorkflow::setSamples(std::shared_ptr<const SampleSet> s) {
  clear();
  // 样本先落位、再清训练集：clearTraining 发 trainingChanged 时 m_samples
  // 已非空，controller 的 refreshTrainingState 才能按新样本算禁用原因
  // （顺序反了会停在「请先读取至少两个通道」的陈旧文案上）。
  m_samples = std::move(s);
  clearTraining(); // 样本换页即清训练集：行号依附旧样本，防错位标注
}
void FaciesClassifyWorkflow::assignTrainingLabel(const QVector<int> &rows,
                                                 const QString &className) {
  if (!m_samples)
    return;
  const QString name = className.trimmed();
  if (name.isEmpty()) {
    emit failed(tr("类名不能为空"));
    return;
  }
  if (m_training.labels.size() != m_samples->rows())
    m_training.labels.assign(m_samples->rows(), -1);
  int code = m_training.classNames.indexOf(name);
  if (code < 0) {
    code = m_training.classNames.size();
    if (code > 254) {
      emit failed(tr("类别数超出分类栅格编码范围"));
      return;
    }
    m_training.classNames << name;
  }
  int applied = 0;
  for (int row : rows) {
    if (row < 0 || std::size_t(row) >= m_samples->rows())
      continue;
    m_training.labels[std::size_t(row)] = code;
    ++applied;
  }
  if (!applied) {
    emit failed(tr("没有有效行号可标注"));
    return;
  }
  m_training.provenance.insert("labelProvenance", "free_text_lasso");
  int labeled = 0;
  for (int c : m_training.labels)
    if (c >= 0)
      ++labeled;
  m_training.provenance.insert("labeledCount", labeled);
  m_training.provenance.insert("classCount", m_training.classNames.size());
  m_trainingDirty = true; // 标注变过——已训练模型即过期，推理前须重训
  emit trainingChanged();
}
void FaciesClassifyWorkflow::clearTraining() {
  // 在途训练任务一并作废：cancel 阻止继续跑，generation 自增幅使其
  // finished 回调直接返回——防训练完成后把已清空的模型复活回 m_trained。
  cancel();
  ++m_generation;
  m_task = nullptr;
  m_training = TrainingSet{};
  m_trained.reset();
  m_trainingDirty = false;
  // 直调 clearTraining 时 finished 回调被 generation 早退，busy 永驻——
  // 与 clear() 的补偿 emit 口径对齐（面板/训练按钮需要回到可用态）。
  emit busyChanged(false);
  emit trainingChanged();
}
QString FaciesClassifyWorkflow::trainingSummary() const {
  if (!m_samples || m_training.labels.size() != m_samples->rows())
    return tr("未标注");
  QVector<int> counts(m_training.classNames.size(), 0);
  int labeled = 0, used = 0;
  for (int code : m_training.labels)
    if (code >= 0 && code < counts.size()) {
      ++counts[code];
      ++labeled;
    }
  if (!labeled)
    return tr("未标注");
  QStringList parts, orphans;
  for (int i = 0; i < counts.size(); ++i) {
    if (counts[i]) {
      ++used; // 类数只数有样本的类，孤立类名单列
      parts << tr("%1：%2 样本")
                   .arg(m_training.classNames[i])
                   .arg(counts[i]);
    } else {
      orphans << m_training.classNames[i];
    }
  }
  QString summary = tr("已标注 %1 样本 / %2 类（%3）")
                        .arg(labeled)
                        .arg(used)
                        .arg(parts.join(tr("；")));
  if (!orphans.isEmpty())
    summary += tr("；未使用的类名：%1").arg(orphans.join(tr("、")));
  return summary;
}
QVariantMap
FaciesClassifyWorkflow::trainingReport(const TrainedModel &m) const {
  QVariantMap report;
  // 键类型口径：supervisedMethod = int(Classifier)（domain 枚举序，
  // 5/6/7 = Lda/Qda/Knn）——与 cluster::SupervisedMethod 同序但是两个不同
  // 枚举，UI 一律读这个；m.provenance 里的同键同值，训练时就落好了。
  report.insert("supervisedMethod",
                m.provenance.value("supervisedMethod").toInt());
  report.insert("method", m.provenance.value("method"));
  report.insert("classNames", m.classNames);
  QVariantList classIds;
  for (int code : m.model.classIds)
    classIds << code;
  report.insert("classIds", classIds);
  report.insert("folds", m.cvFolds);
  report.insert("labeledCount",
                m.provenance.value("labeledCount").toInt());
  // 混淆矩阵 cells 平铺（行主序 c×c），配合 classIds 复原。
  QVariantList cells;
  for (const auto &row : m.cv.cells)
    for (std::int64_t v : row)
      cells << qint64(v);
  report.insert("confusionCells", cells);
  QVariantList precision, recall;
  for (double v : m.cv.precision)
    precision << v;
  for (double v : m.cv.recall)
    recall << v;
  report.insert("precision", precision);
  report.insert("recall", recall);
  report.insert("trainingSetHash", m.provenance.value("trainingSetHash"));
  report.insert("provenance", m.provenance);
  return report;
}
void FaciesClassifyWorkflow::train(const ClassificationOptions &o) {
  if (!m_tasks || !m_samples) {
    emit failed(tr("没有任务服务或交会样本"));
    return;
  }
  const auto precondition =
      FaciesTrainingService::validateTrainingSet(*m_samples, m_training, o);
  if (!precondition.ok) {
    emit failed(precondition.error);
    return;
  }
  cancel();
  const auto generation = ++m_generation;
  auto samples = m_samples;
  auto training = std::make_shared<TrainingSet>(m_training);
  auto trained = std::make_shared<TrainedModel>();
  QStringList warnings = precondition.warnings;
  emit busyChanged(true);
  auto *task = m_tasks->start(
      tr("训练交会监督分类模型"),
      [samples, training, o, trained](PaleoTask *task) {
        cluster::Control ctl{
            [task] { return task->cancelRequested(); },
            [task](double p) { task->reportBytes(qint64(p * 1000), 1000); }};
        QString error;
        if (FaciesTrainingService::train(*samples, *training, o, trained.get(),
                                         &error, ctl))
          return QString();
        return error.isEmpty() ? tr("训练失败") : error;
      });
  m_task = task;
  connect(task, &PaleoTask::changed, this, [this, task, generation] {
    if (generation == m_generation)
      emit progressChanged(task->percent());
  });
  connect(task, &PaleoTask::finished, this,
          [this, task, trained, warnings, generation] {
            if (generation != m_generation)
              return;
            m_task = nullptr;
            emit busyChanged(false);
            if (task->state() == PaleoTask::State::Cancelled) {
              emit failed(tr("训练已取消，未生成模型"));
              return;
            }
            if (task->state() != PaleoTask::State::Succeeded) {
              emit failed(task->errorText().isEmpty()
                              ? tr("训练失败")
                              : task->errorText());
              return;
            }
            m_trained = trained;
            m_trainingDirty = false; // 模型与当前标注重新对齐
            QVariantMap report = trainingReport(*trained);
            report.insert("warnings", warnings);
            emit trainingReady(report);
          });
}
void FaciesClassifyWorkflow::cancel() {
  if (m_task)
    m_task->requestCancel();
}
PaleoTask *FaciesClassifyWorkflow::classify(const ClassificationOptions &o) {
  if (!m_tasks || !m_samples) {
    emit failed(tr("没有任务服务或交会样本"));
    return nullptr;
  }
  const bool supervised = isSupervisedClassifier(o.method);
  if (supervised) {
    if (!m_trained) {
      emit failed(tr("尚未训练监督模型，请先训练"));
      return nullptr;
    }
    if (m_trained->model.method != supervisedMethodOf(o.method)) {
      emit failed(tr("监督方法需先训练（当前模型与所选方法不一致）"));
      return nullptr;
    }
    if (m_trainingDirty) {
      emit failed(tr("训练集已变更，请重新训练后再推理"));
      return nullptr;
    }
  }
  cancel();
  const auto generation = ++m_generation;
  auto samples = m_samples;
  auto previous = std::make_shared<Classification>(
      o.method == Classifier::Hull || o.method == Classifier::Box
          ? m_classification
          : Classification{});
  auto result = std::make_shared<Classification>();
  auto trained = m_trained;
  m_options = o;
  emit busyChanged(true);
  auto *task = m_tasks->start(
      supervised ? tr("交会有监督相分类") : tr("交会无监督相分类"),
      [samples, o, previous, result, supervised, trained](PaleoTask *task) {
        cluster::Control ctl{
            [task] { return task->cancelRequested(); },
            [task](double p) { task->reportBytes(qint64(p * 1000), 1000); }};
        *result = supervised
                      ? FaciesTrainingService::classifyWith(
                            *samples, *trained, o, ctl)
                      : FaciesClassificationService::classify(*samples, o, ctl,
                                                              *previous);
        return result->cancelled ? QString() : result->error;
      });
  m_task = task;
  connect(task, &PaleoTask::changed, this, [this, task, generation] {
    if (generation == m_generation)
      emit progressChanged(task->percent());
  });
  connect(task, &PaleoTask::finished, this, [this, task, result, generation] {
    if (generation != m_generation)
      return;
    m_task = nullptr;
    emit busyChanged(false);
    if (result->ok && task->state() == PaleoTask::State::Succeeded) {
      m_classification = *result;
      emit classificationReady(m_classification);
    } else
      emit failed(
          task->state() == PaleoTask::State::Cancelled
              ? tr("分类已取消，未生成新成果")
              : (result->error.isEmpty() ? task->errorText() : result->error));
  });
  return task;
}
FaciesProduct FaciesClassifyWorkflow::write(const QString &horizon) {
  FaciesProduct out;
  auto fail = [&](const QString &e) {
    out.error = e;
    emit failed(e);
    return out;
  };
  if (m_task && m_task->running())
    return fail(tr("分类仍在运行"));
  if (!m_catalog || m_catalog->refusesWrites() || !m_store ||
      m_store->isReadOnly() || m_projectDir.isEmpty() || !m_samples ||
      !m_classification.ok)
    return fail(tr("工程不可写或没有分类成果"));
  const auto &s = *m_samples;
  const auto &r = m_classification;
  const bool raster = s.grid.spatial && s.grid.rows > 0;
  if (raster && (!m_layers || horizon.isEmpty()))
    return fail(tr("栅格写回需要有效层位和图层服务"));
  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  QString error;
  const auto stage = registrar.stage(
      raster ? QStringLiteral("facies_classification")
             : QStringLiteral("well_facies_intervals"),
      raster ? tr("%1 交会分类").arg(horizon) : tr("交会井层段分类"),
      raster ? QStringLiteral("facies.tif")
             : QStringLiteral("well-facies.json"),
      &error);
  if (!stage.isValid())
    return fail(error);
  auto written = m_store->enqueueWrite([&] {
    const bool ok = raster ? FaciesClassificationService::writeRaster(
                                 stage.absolutePath, s, r, &error)
                           : FaciesClassificationService::writeIntervals(
                                 stage.absolutePath, s, r, &error);
    return PaleoProjectStore::WriteResult{ok, error};
  });
  if (!written.ok) {
    QFile::remove(stage.absolutePath);
    return fail(written.error);
  }
  if (!raster) {
    // 井层段路径：只 intervals 一件，行为保持现状（无伴生栅格）。
    QVariantMap extra = r.provenance;
    extra.insert("horizon", horizon);
    extra.insert("intervalSupport",
                 "inclusive_observed_depths_no_gap_bridging");
    if (!registrar.commit(stage, s.parentVersionIds,
                          QStringLiteral("crossplot://classification"), extra,
                          &error)) {
      QFile::remove(stage.absolutePath);
      return fail(error);
    }
    out.path = stage.absolutePath;
    out.assetId = stage.assetId;
    out.versionId = stage.versionId;
    out.counts = r.counts;
    QStringList wells;
    for (const auto &loc : s.locations)
      if (!loc.wellId.isEmpty() && !wells.contains(loc.wellId))
        wells << loc.wellId;
    for (const auto &well : wells) {
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.entityId = well;
      link.assetId = out.assetId;
      link.role = QStringLiteral("interpretation");
      link.note = tr("交会分类井层段");
      if (!m_catalog->addLink(link, &error))
        return fail(tr("成果已登记，但井关联失败：%1").arg(error));
    }
    out.ok = true;
    emit productReady(out);
    return out;
  }
  // 栅格路径三件套：主分类图 + 置信度件（Float32）+ 低置信掩膜件（Byte）。
  // 先全部 stage + 写盘成功，再统一逐个 commit——写盘失败清理全部已写
  // 文件、catalog 零登记（不留半套孤儿）；commit 失败无法回滚已登记件
  // （无单版本级、非 stale、不带 collateral 的删除面），只能如实告知哪些
  // 件已入 catalog、未声明图层，并指向重试路径。两类失败均有负路径测试：
  // 写盘失败 = NaN 掩膜阈值；commit 失败 = warm catalog 下持
  // CatalogPurgeLease（addVersion 的 checkWriteThread 拒写）。
  QString suffix = horizon;
  suffix.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                 QStringLiteral("_"));
  out.layerId = QStringLiteral("predict.%1.crossplot").arg(suffix);
  struct Companion {
    QString assetType, layerId, title, what;
    bool masked;
  };
  const Companion companions[] = {
      {QStringLiteral("confidence"),
       QStringLiteral("confidence.%1.crossplot").arg(suffix),
       tr("%1 分类置信度").arg(horizon), tr("置信度"), false},
      {QStringLiteral("facies_classification_masked"),
       QStringLiteral("predict.%1.crossplot.masked").arg(suffix),
       tr("%1 交会分类（低置信掩膜）").arg(horizon), tr("低置信掩膜"), true},
  };
  const double threshold = m_options.confidenceMaskThreshold;
  QVector<DerivedStaging> staged{stage};
  QVector<QString> stagedPaths{stage.absolutePath};
  for (const auto &c : companions) {
    const auto cstage = registrar.stage(
        c.assetType, c.title, c.assetType + QStringLiteral(".tif"), &error);
    if (!cstage.isValid()) {
      for (const auto &path : stagedPaths)
        QFile::remove(path);
      return fail(error);
    }
    staged << cstage;
    stagedPaths << cstage.absolutePath;
  }
  auto companionWrite = [&](const Companion &c, const DerivedStaging &st) {
    return m_store->enqueueWrite([&] {
      const bool ok =
          c.masked ? FaciesClassificationService::writeMaskedRaster(
                         st.absolutePath, s, r, threshold, &error)
                   : FaciesClassificationService::writeConfidenceRaster(
                         st.absolutePath, s, r, &error);
      return PaleoProjectStore::WriteResult{ok, error};
    });
  };
  for (int i = 1; i < staged.size(); ++i) {
    const auto result = companionWrite(companions[i - 1], staged[i]);
    if (!result.ok) {
      for (const auto &path : stagedPaths)
        QFile::remove(path);
      return fail(tr("%1伴生栅格写入失败：%2")
                      .arg(companions[i - 1].what, result.error));
    }
  }
  // 三件全部落盘，逐个 commit；commit 失败时清掉尚未登记的文件。
  auto commitExtra = [&](const QString &layerId) {
    QVariantMap extra = r.provenance;
    extra.insert("horizon", horizon);
    extra.insert("layer_id", layerId);
    return extra;
  };
  if (!registrar.commit(stage, s.parentVersionIds,
                        QStringLiteral("crossplot://classification"),
                        commitExtra(out.layerId), &error)) {
    for (const auto &path : stagedPaths)
      QFile::remove(path);
    return fail(tr("主分类图登记失败：%1（尚未登记任何版本）").arg(error));
  }
  out.path = stage.absolutePath;
  out.assetId = stage.assetId;
  out.versionId = stage.versionId;
  out.counts = r.counts;
  for (int i = 1; i < staged.size(); ++i) {
    const auto &c = companions[i - 1];
    const auto &st = staged[i];
    QVariantMap extra = commitExtra(c.layerId);
    if (c.masked)
      extra.insert("confidenceMaskThreshold", threshold);
    if (!registrar.commit(st, s.parentVersionIds,
                          QStringLiteral("crossplot://classification"), extra,
                          &error)) {
      for (int j = i; j < staged.size(); ++j)
        QFile::remove(staged[j].absolutePath);
      // commit 失败不伪造「零残留」：主分类图（及已成功的伴生件）已在
      // catalog，只是还没声明图层——如实列出并给出重试路径（重跑本层位
      // 写入会按资产递增下一版本，不会覆盖已登记件）。
      QStringList registered{tr("主分类图")};
      for (int j = 1; j < i; ++j)
        registered << companions[j - 1].what;
      return fail(tr("%1伴生栅格登记失败：%2；%3已登记入 catalog 但未声明图层，"
                     "可重试本层位写入生成下一版本")
                      .arg(c.what, error, registered.join(tr("、"))));
    }
    if (c.masked) {
      out.maskedPath = st.absolutePath;
      out.maskedAssetId = st.assetId;
      out.maskedVersionId = st.versionId;
      out.maskedLayerId = c.layerId;
    } else {
      out.confidencePath = st.absolutePath;
      out.confidenceAssetId = st.assetId;
      out.confidenceVersionId = st.versionId;
      out.confidenceLayerId = c.layerId;
    }
  }
  // 图层声明；失败文案附已登记 layer_id 清单（含主图）。
  QStringList declared;
  auto declareOne = [&](const QString &layerId, const QString &title,
                        const QString &source, const QString &what) {
    LayerDeclaration declaration;
    declaration.layerId = layerId;
    declaration.horizon = horizon;
    declaration.type = QStringLiteral("raster");
    declaration.source = source;
    declaration.group = QStringLiteral("02_Prediction");
    declaration.title = title;
    if (!m_layers->declare(declaration, &error)) {
      // 空清单不带空括注（主图声明失败时清单本就为空）。
      const QString registered =
          declared.isEmpty()
              ? QString()
              : tr("（已登记图层：%1）").arg(declared.join(QStringLiteral("、")));
      fail(tr("成果已登记，但%1图层声明失败：%2%3").arg(what, error, registered));
      return false;
    }
    declared << layerId;
    return true;
  };
  if (!declareOne(out.layerId, tr("%1 交会分类（簇编号）").arg(horizon),
                  out.path, tr("主分类")))
    return out;
  for (const auto &c : companions) {
    const auto &source = c.masked ? out.maskedPath : out.confidencePath;
    if (!declareOne(c.layerId, c.title, source, c.what))
      return out;
  }
  out.ok = true;
  emit productReady(out);
  return out;
}
} // namespace paleo::crossplot
