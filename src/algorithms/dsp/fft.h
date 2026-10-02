// 层：数据
#pragma once

// dsp — 数字信号基础件（纯数值，无 Qt/GIS）。
// radix-2 FFT 为自研迭代 Cooley-Tukey（double 内部精度）：仓库 vendor/
// 系统/Qt 均无合规 FFT（FFTW double-only 头且未链接、破坏钉位策略，见
// .goal-loop-ledger-seismic-attributes.md 轮0），自写是零新依赖的唯一路。
// 原在 seismicattr.cpp 文件内，seismic-inversion 轮0 提出共享。

namespace paleo::dsp
{

// 迭代 Cooley-Tukey radix-2，原地。n 必须为 2 的幂。
// forward: X[k] = Σ x[j]·e^{-i2πjk/n}；inverse 先逆变换再整体除 n。
void fftRadix2(double *re, double *im, int n, bool inverse);

// ≥v 的最小 2 的幂（v ≤ 1 → 1）。
int nextPowerOfTwoAtLeast(int v);

} // namespace paleo::dsp
