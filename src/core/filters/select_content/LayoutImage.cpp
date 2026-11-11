// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "LayoutImage.h"

#include <QImage>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace select_content {
namespace layout {
namespace {
// cvRound: round half to even (lrint in the default rounding mode).
inline int roundEven(double v) {
  return static_cast<int>(std::nearbyint(v));
}

inline uint8_t saturate8(int v) {
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// BORDER_REFLECT_101: ...2 1 | 0 1 2 ... n-1 | n-2 n-3...
inline int reflect101(int i, int n) {
  if (n == 1) {
    return 0;
  }
  while (i < 0 || i >= n) {
    if (i < 0) {
      i = -i;
    }
    if (i >= n) {
      i = 2 * n - 2 - i;
    }
  }
  return i;
}

struct AreaTab {
  int di;
  int si;
  float alpha;
};

// cv::computeResizeAreaTab
std::vector<AreaTab> resizeAreaTab(int ssize, int dsize, double scale) {
  std::vector<AreaTab> tab;
  for (int dx = 0; dx < dsize; ++dx) {
    const double fsx1 = dx * scale;
    const double fsx2 = fsx1 + scale;
    const double cellWidth = std::min(scale, ssize - fsx1);
    int sx1 = static_cast<int>(std::ceil(fsx1));
    int sx2 = static_cast<int>(std::floor(fsx2));
    sx2 = std::min(sx2, ssize - 1);
    sx1 = std::min(sx1, sx2);
    if (sx1 - fsx1 > 1e-3) {
      tab.push_back({dx, sx1 - 1, static_cast<float>((sx1 - fsx1) / cellWidth)});
    }
    for (int sx = sx1; sx < sx2; ++sx) {
      tab.push_back({dx, sx, static_cast<float>(1.0 / cellWidth)});
    }
    if (fsx2 - sx2 > 1e-3) {
      tab.push_back({dx, sx2, static_cast<float>(std::min(std::min(fsx2 - sx2, 1.), cellWidth) / cellWidth)});
    }
  }
  return tab;
}

// cv::resize INTER_AREA when both scale factors are >= 1 (resizeArea_).
Plane8 resizeAreaDown(const Plane8& src, int w, int h) {
  const int cn = src.channels;
  const std::vector<AreaTab> xtab = resizeAreaTab(src.width, w, static_cast<double>(src.width) / w);
  const std::vector<AreaTab> ytab = resizeAreaTab(src.height, h, static_cast<double>(src.height) / h);
  Plane8 dst(w, h, cn);
  std::vector<float> sum(static_cast<size_t>(w) * cn);
  std::vector<float> buf(static_cast<size_t>(w) * cn);
  size_t j = 0;
  for (int dy = 0; dy < h; ++dy) {
    std::fill(sum.begin(), sum.end(), 0.f);
    for (; j < ytab.size() && ytab[j].di == dy; ++j) {
      std::fill(buf.begin(), buf.end(), 0.f);
      const uint8_t* s = src.row(ytab[j].si);
      for (const AreaTab& t : xtab) {
        for (int c = 0; c < cn; ++c) {
          buf[t.di * cn + c] += s[t.si * cn + c] * t.alpha;
        }
      }
      const float beta = ytab[j].alpha;
      for (size_t i = 0; i < sum.size(); ++i) {
        sum[i] += buf[i] * beta;
      }
    }
    uint8_t* d = dst.row(dy);
    for (size_t i = 0; i < sum.size(); ++i) {
      d[i] = saturate8(roundEven(sum[i]));
    }
  }
  return dst;
}

struct LinearCoeffs {
  std::vector<int> ofs;     // source index
  std::vector<short> a0;    // weight of ofs, 11-bit fixed point
  std::vector<short> a1;    // weight of ofs + 1
  int max = 0;              // from here on: one tap (the last source pixel)
};

// cv::resize's coefficient set-up for INTER_AREA in its generic (2-tap) path.
LinearCoeffs areaCoeffs(int ssize, int dsize) {
  const double invScale = static_cast<double>(dsize) / ssize;
  const double scale = 1.0 / invScale;
  LinearCoeffs k;
  k.ofs.resize(dsize);
  k.a0.resize(dsize);
  k.a1.resize(dsize);
  k.max = dsize;
  for (int dx = 0; dx < dsize; ++dx) {
    int sx = static_cast<int>(std::floor(dx * scale));
    float fx = static_cast<float>((dx + 1) - (sx + 1) * invScale);
    fx = fx <= 0 ? 0.f : fx - std::floor(fx);
    if (sx < 0) {
      fx = 0;
      sx = 0;
    }
    if (sx + 1 >= ssize) {
      k.max = std::min(k.max, dx);
      if (sx >= ssize - 1) {
        fx = 0;
        sx = ssize - 1;
      }
    }
    k.ofs[dx] = sx;
    k.a0[dx] = static_cast<short>(std::clamp(roundEven((1.f - fx) * 2048.f), -32768, 32767));
    k.a1[dx] = static_cast<short>(std::clamp(roundEven(fx * 2048.f), -32768, 32767));
  }
  return k;
}

// cv::resize INTER_AREA with a scale factor < 1 in one direction: OpenCV's generic separable
// path with the "area" 2-tap coefficients, fixed point (8U), vertical pass as its SIMD code.
Plane8 resizeAreaGeneric(const Plane8& src, int w, int h) {
  const int cn = src.channels;
  const LinearCoeffs kx = areaCoeffs(src.width, w);
  const LinearCoeffs ky = areaCoeffs(src.height, h);
  // Horizontal pass of every source row that is needed, into int rows (scale 2048).
  auto hresize = [&](int sy, std::vector<int>& out) {
    const uint8_t* s = src.row(sy);
    for (int dx = 0; dx < w; ++dx) {
      const int sx = kx.ofs[dx];
      for (int c = 0; c < cn; ++c) {
        if (dx < kx.max) {
          out[dx * cn + c] = s[sx * cn + c] * kx.a0[dx] + s[(sx + 1) * cn + c] * kx.a1[dx];
        } else {
          out[dx * cn + c] = s[sx * cn + c] * 2048;
        }
      }
    }
  };
  Plane8 dst(w, h, cn);
  std::vector<int> r0(static_cast<size_t>(w) * cn), r1(static_cast<size_t>(w) * cn);
  for (int dy = 0; dy < h; ++dy) {
    const int sy0 = ky.ofs[dy];
    const int sy1 = std::min(sy0 + 1, src.height - 1);
    hresize(sy0, r0);
    hresize(sy1, r1);
    const int b0 = ky.a0[dy];
    const int b1 = ky.a1[dy];
    uint8_t* d = dst.row(dy);
    for (size_t i = 0; i < r0.size(); ++i) {
      const int s0 = std::clamp(r0[i] >> 4, -32768, 32767);
      const int s1 = std::clamp(r1[i] >> 4, -32768, 32767);
      const int v = ((s0 * b0) >> 16) + ((s1 * b1) >> 16);
      d[i] = saturate8((v + 2) >> 2);
    }
  }
  return dst;
}

template <typename Get>
std::vector<double> blurPass(int n, const std::vector<double>& k, Get get) {
  const int r = static_cast<int>(k.size()) / 2;
  std::vector<double> padded(n + 2 * r);
  for (int i = 0; i < n + 2 * r; ++i) {
    padded[i] = get(reflect101(i - r, n));
  }
  std::vector<double> out(n);
  const int ks = static_cast<int>(k.size());
  for (int i = 0; i < n; ++i) {
    const double* p = padded.data() + i;
    double s = 0;
    for (int j = 0; j < ks; ++j) {
      s += k[j] * p[j];
    }
    out[i] = s;
  }
  return out;
}

template <typename In, typename Out, typename Store>
void separableBlur(const In& src, Out& dst, const std::vector<double>& k, Store store) {
  const int w = src.width;
  const int h = src.height;
  std::vector<double> tmp(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const std::vector<double> row = blurPass(w, k, [&](int x) { return static_cast<double>(src.at(x, y)); });
    std::copy(row.begin(), row.end(), tmp.begin() + static_cast<size_t>(y) * w);
  }
  for (int x = 0; x < w; ++x) {
    const std::vector<double> col = blurPass(h, k, [&](int y) { return tmp[static_cast<size_t>(y) * w + x]; });
    for (int y = 0; y < h; ++y) {
      store(dst, x, y, col[y]);
    }
  }
}
}  // namespace

Plane8 warpAffineRgb(const QImage& srcImage,
                     const double m[6],
                     const int dstWidth,
                     const int dstHeight,
                     const uint8_t border) {
  QImage src = srcImage;
  const bool gray = src.format() == QImage::Format_Grayscale8;
  if (!gray && src.format() != QImage::Format_RGB32 && src.format() != QImage::Format_ARGB32
      && src.format() != QImage::Format_ARGB32_Premultiplied) {
    src = src.convertToFormat(QImage::Format_RGB32);
  }
  const int sw = src.width();
  const int sh = src.height();
  auto pixel = [&](int x, int y, uint8_t* rgb) {
    if (x < 0 || y < 0 || x >= sw || y >= sh) {
      rgb[0] = rgb[1] = rgb[2] = border;
    } else if (gray) {
      rgb[0] = rgb[1] = rgb[2] = src.constScanLine(y)[x];
    } else {
      const QRgb p = reinterpret_cast<const QRgb*>(src.constScanLine(y))[x];
      rgb[0] = static_cast<uint8_t>(qRed(p));
      rgb[1] = static_cast<uint8_t>(qGreen(p));
      rgb[2] = static_cast<uint8_t>(qBlue(p));
    }
  };

  // cv::warpAffine: coordinates in 1/1024 px, rounded to 1/32 px (INTER_BITS = 5),
  // weights in 15-bit fixed point.
  constexpr int kAbScale = 1024;
  constexpr int kRoundDelta = kAbScale / 32 / 2;
  std::vector<int> adelta(dstWidth), bdelta(dstWidth);
  for (int x = 0; x < dstWidth; ++x) {
    adelta[x] = roundEven(m[0] * x * kAbScale);
    bdelta[x] = roundEven(m[3] * x * kAbScale);
  }
  Plane8 dst(dstWidth, dstHeight, 3);
  uint8_t p[4][3];
  for (int y = 0; y < dstHeight; ++y) {
    const int x0 = roundEven((m[1] * y + m[2]) * kAbScale) + kRoundDelta;
    const int y0 = roundEven((m[4] * y + m[5]) * kAbScale) + kRoundDelta;
    uint8_t* d = dst.row(y);
    for (int x = 0; x < dstWidth; ++x) {
      const int X = (x0 + adelta[x]) >> 5;
      const int Y = (y0 + bdelta[x]) >> 5;
      const int sx = X >> 5;
      const int sy = Y >> 5;
      const int fx = X & 31;
      const int fy = Y & 31;
      if (sx >= sw || sx + 1 < 0 || sy >= sh || sy + 1 < 0) {
        d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = border;
        continue;
      }
      const int w00 = (32 - fx) * (32 - fy) * 32;
      const int w01 = fx * (32 - fy) * 32;
      const int w10 = (32 - fx) * fy * 32;
      const int w11 = fx * fy * 32;
      pixel(sx, sy, p[0]);
      pixel(sx + 1, sy, p[1]);
      pixel(sx, sy + 1, p[2]);
      pixel(sx + 1, sy + 1, p[3]);
      for (int c = 0; c < 3; ++c) {
        const int v = p[0][c] * w00 + p[1][c] * w01 + p[2][c] * w10 + p[3][c] * w11;
        d[3 * x + c] = saturate8((v + (1 << 14)) >> 15);
      }
    }
  }
  return dst;
}

void fillOutsideConvexPolygon(Plane8& img, const std::vector<double>& poly, const uint8_t value) {
  const size_t n = poly.size() / 2;
  if (n < 3) {
    return;
  }
  std::vector<double> px(n), py(n);
  for (size_t i = 0; i < n; ++i) {
    px[i] = std::nearbyint(poly[2 * i]);
    py[i] = std::nearbyint(poly[2 * i + 1]);
  }
  for (int y = 0; y < img.height; ++y) {
    double xl = 1e300, xr = -1e300;
    for (size_t i = 0; i < n; ++i) {
      const size_t j = (i + 1) % n;
      const double ya = py[i], yb = py[j];
      if (y < std::min(ya, yb) || y > std::max(ya, yb)) {
        continue;
      }
      if (ya == yb) {
        xl = std::min({xl, px[i], px[j]});
        xr = std::max({xr, px[i], px[j]});
      } else {
        const double x = px[i] + (y - ya) * (px[j] - px[i]) / (yb - ya);
        xl = std::min(xl, x);
        xr = std::max(xr, x);
      }
    }
    uint8_t* r = img.row(y);
    for (int x = 0; x < img.width; ++x) {
      if (x < std::ceil(xl - 1e-9) || x > std::floor(xr + 1e-9)) {
        std::fill(r + x * img.channels, r + (x + 1) * img.channels, value);
      }
    }
  }
}

Plane8 resizeArea(const Plane8& src, const int w, const int h) {
  if (w <= 0 || h <= 0 || src.isNull()) {
    throw std::invalid_argument("resizeArea: empty image");
  }
  if (src.width >= w && src.height >= h) {
    return resizeAreaDown(src, w, h);
  }
  return resizeAreaGeneric(src, w, h);
}

Plane8 rgbToGray(const Plane8& rgb) {
  Plane8 g(rgb.width, rgb.height, 1);
  for (int y = 0; y < rgb.height; ++y) {
    const uint8_t* s = rgb.row(y);
    uint8_t* d = g.row(y);
    for (int x = 0; x < rgb.width; ++x) {
      d[x] = static_cast<uint8_t>((9797 * s[3 * x] + 19234 * s[3 * x + 1] + 3737 * s[3 * x + 2] + 16384) >> 15);
    }
  }
  return g;
}

std::vector<double> gaussianKernel(const int n, const double sigma) {
  std::vector<double> k(n);
  const double scale2X = -0.5 / (sigma * sigma);
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    const double x = i - (n - 1) * 0.5;
    k[i] = std::exp(scale2X * x * x);
    sum += k[i];
  }
  for (double& v : k) {
    v /= sum;
  }
  return k;
}

Plane8 gaussianBlur(const Plane8& src, const double sigma) {
  const int n = roundEven(sigma * 3 * 2 + 1) | 1;
  Plane8 dst(src.width, src.height, 1);
  separableBlur(src, dst, gaussianKernel(n, sigma),
                [](Plane8& d, int x, int y, double v) { d.row(y)[x] = saturate8(roundEven(v)); });
  return dst;
}

PlaneF gaussianBlur(const PlaneF& src, const double sigma) {
  const int n = roundEven(sigma * 4 * 2 + 1) | 1;
  PlaneF dst(src.width, src.height);
  separableBlur(src, dst, gaussianKernel(n, sigma),
                [](PlaneF& d, int x, int y, double v) { d.at(x, y) = static_cast<float>(v); });
  return dst;
}

double percentile(std::vector<float>& values, const double q) {
  if (values.empty()) {
    return std::nan("");
  }
  const double idx = q / 100.0 * (values.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = std::min(lo + 1, values.size() - 1);
  std::nth_element(values.begin(), values.begin() + lo, values.end());
  const double a = values[lo];
  double b = a;
  if (hi != lo) {
    b = *std::min_element(values.begin() + lo + 1, values.end());
  }
  const double t = idx - lo;
  // numpy's _lerp
  return t >= 0.5 ? b - (b - a) * (1 - t) : a + (b - a) * t;
}
}  // namespace layout
}  // namespace select_content
