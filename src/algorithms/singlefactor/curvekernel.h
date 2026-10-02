// 层：数据
#pragma once

#include "types.h"

#include <array>
#include <vector>

// 层：数据
namespace paleo::singlefactor
{

// 紧支撑 C2 曲线核。沿折线离散积分切向张量，不挑选最近线段。
// 线反向不改变核：中点集合与 dyad（切向平方、叉积）相同。
class CurveKernel
{
  public:
    CurveKernel( const std::vector<Point2> &points, double radius, double core );

    double radius() const { return m_radius; }
    double coreMass() const { return m_coreMass; }
    int centerCount() const { return static_cast<int>( m_centers.size() ); }

    struct Eval
    {
      std::vector<double> gate;
      std::vector<std::array<double, 3>> tensor;
    };

    Eval evaluate( const std::vector<Point2> &points ) const;
    void evaluateInto( const Point2 *points, int count, std::vector<double> &gate,
                       std::vector<std::array<double, 3>> &tensor ) const;

    static double basis( double r );

  private:
    double m_radius = 1;
    double m_coreMass = 1e-20;
    std::vector<Point2> m_centers;
    std::vector<double> m_weights;
    std::vector<std::array<double, 3>> m_dyads;
};

double tangentEnergy( double dx, double dy, const std::array<double, 3> &tensor );

} // namespace paleo::singlefactor
