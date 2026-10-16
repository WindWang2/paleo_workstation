// 层：数据
#include "fft.h"

#include <cmath>
#include <utility>
#include <vector>

namespace paleo::dsp
{

void fftRadix2(double *re, double *im, int n, bool inverse)
{
  for (int i = 1, j = 0; i < n; ++i)
  {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
    {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  // #478：旋转因子逐项直接三角求值成表（exp(±i2πk/n)，k < n/2），蝶形按
  // k·(n/len) 步长查表。原实现块内复乘递推 (cr,ci)·(wr,wi) 的舍入误差沿
  // k 链线性累积（末级链长 n/2，n=2^20 时 twiddle 相对误差 ~2e-11），大
  // nFft 的谱值偏离直接求值；查表后误差只剩蝶形算术 O(eps)。表成本
  // n/2 次 cos/sin（与原一次递推种子同量级），内存 n 个 double。
  const int half = n / 2;
  std::vector<double> twiddleR(static_cast<std::size_t>( std::max( half, 1 ) ), 1.0);
  std::vector<double> twiddleI(static_cast<std::size_t>( std::max( half, 1 ) ), 0.0);
  for (int k = 0; k < half; ++k)
  {
    const double ang = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 * double(k) / double(n);
    twiddleR[static_cast<std::size_t>( k )] = std::cos(ang);
    twiddleI[static_cast<std::size_t>( k )] = std::sin(ang);
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    const int stride = n / len;
    for (int base = 0; base < n; base += len)
    {
      for (int k = 0; k < len / 2; ++k)
      {
        const int a = base + k;
        const int b = a + len / 2;
        const double cr = twiddleR[static_cast<std::size_t>( k ) * stride];
        const double ci = twiddleI[static_cast<std::size_t>( k ) * stride];
        const double tr = re[b] * cr - im[b] * ci;
        const double ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
    }
  }
  if (inverse)
  {
    const double inv = 1.0 / double(n);
    for (int i = 0; i < n; ++i)
    {
      re[i] *= inv;
      im[i] *= inv;
    }
  }
}

int nextPowerOfTwoAtLeast(int v)
{
  int p = 1;
  while (p < v)
    p <<= 1;
  return p;
}

} // namespace paleo::dsp
