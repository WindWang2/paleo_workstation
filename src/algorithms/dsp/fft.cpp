// 层：数据
#include "fft.h"

#include <cmath>
#include <utility>

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
  for (int len = 2; len <= n; len <<= 1)
  {
    const double ang = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 / double(len);
    const double wr = std::cos(ang);
    const double wi = std::sin(ang);
    for (int base = 0; base < n; base += len)
    {
      double cr = 1.0;
      double ci = 0.0;
      for (int k = 0; k < len / 2; ++k)
      {
        const int a = base + k;
        const int b = a + len / 2;
        const double tr = re[b] * cr - im[b] * ci;
        const double ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
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
