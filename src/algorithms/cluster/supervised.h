// 层：数据
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cluster.h"

namespace paleo::cluster {
enum class SupervisedMethod { Lda, Qda, Knn };
struct SupervisedOptions {
  SupervisedMethod method = SupervisedMethod::Lda;
  int knnNeighbors = 5;
  double varianceFloor = 1e-6; // covariance ridge; ignored by kNN
};
struct SupervisedModel {
  SupervisedMethod method = SupervisedMethod::Lda;
  std::size_t dimensions = 0;
  std::vector<int> classIds;        // sorted, unique training label codes
  Matrix means;                     // c x d (LDA/QDA); empty for kNN
  std::vector<double> covariances;  // LDA: one pooled d x d block, row-major;
                                    // QDA: c blocks d x d; empty for kNN
  std::vector<double> priors;       // c class proportions
  Matrix trainValues;               // kNN only: m x d labeled rows
  std::vector<int> trainLabels;     // kNN only: m, index into classIds
  int knnNeighbors = 0;             // kNN only: effective k, clamped to the
                                    // labeled row count at train time
};
struct SupervisedResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  SupervisedModel model;
  // trainSupervised: parallel to the labeled rows, in their original relative
  // order (rows labeled -1 are skipped); predictSupervised: parallel to all
  // input rows. Values are indices into model.classIds.
  std::vector<int> labels;
  // LDA/QDA: maximum plug-in posterior (class parameters estimated from the
  // labeled rows, then substituted); kNN: winning-class vote share.
  std::vector<double> confidence; // [0,1]
  std::vector<double>
      squaredDistance; // LDA/QDA squared Mahalanobis distance to the winning
                       // class; kNN mean squared distance of the used neighbors
};
// Trains on the rows labeled with a value other than -1 and classifies those
// rows with the fitted model (in-sample fit, no held-out data involved).
SupervisedResult trainSupervised(const Matrix &,
                                 const std::vector<int> &labels,
                                 const SupervisedOptions & = {},
                                 const Control & = {});
SupervisedResult predictSupervised(const Matrix &,
                                   const SupervisedModel &,
                                   const Control & = {});
struct ConfusionMatrix {
  std::vector<int> classIds;
  std::vector<std::vector<std::int64_t>> cells; // [true][predicted] counts
  std::vector<double> precision, recall;        // per class; NaN when undefined:
                                                // recall needs at least one true
                                                // instance, precision at least
                                                // one prediction (stratified
                                                // folds always contain every
                                                // class, so recall is defined
                                                // and only precision can be NaN)
};
struct CrossValidationResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  ConfusionMatrix confusion;
  int folds = 0;
};
// Stratified k-fold cross-validation over the labeled rows only. Fold
// assignment shuffles each class with std::mt19937_64(seed), so identical
// inputs produce identical output. The requested fold count is clamped to
// the smallest class sample count, because a class too small to populate
// every fold cannot be stratified.
CrossValidationResult crossValidate(const Matrix &,
                                    const std::vector<int> &labels,
                                    const SupervisedOptions &, int folds = 5,
                                    std::uint64_t seed = 42,
                                    const Control & = {});
} // namespace paleo::cluster
