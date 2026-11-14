// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_SELECT_CONTENT_LAYOUTIMAGE_H_
#define SCANTAILOR_SELECT_CONTENT_LAYOUTIMAGE_H_

#include <cstdint>
#include <vector>

class QImage;

namespace select_content {
namespace layout {
/**
 * A plain 8-bit image (1 or 3 interleaved channels) for the layout detector.
 *
 * The detector's model was trained, and its rules tuned, on pages prepared with OpenCV in
 * Python (scantailor-cli: ml/render_pages.py, ml/onnx_predict.py, ml/rules.py). The resampling
 * and filter functions here follow OpenCV's definitions (pixel centres at integer coordinates,
 * cv::warpAffine bilinear with 1/32 px sub-pixel steps, cv::resize INTER_AREA, cv::GaussianBlur
 * kernel size and BORDER_REFLECT_101), so that the model sees the same input and the rules
 * give the same boxes as the Python reference.
 */
struct Plane8 {
  int width = 0;
  int height = 0;
  int channels = 1;
  std::vector<uint8_t> data;

  Plane8() = default;

  Plane8(int w, int h, int ch, uint8_t fill = 0)
      : width(w), height(h), channels(ch), data(static_cast<size_t>(w) * h * ch, fill) {}

  bool isNull() const { return data.empty(); }

  uint8_t* row(int y) { return data.data() + static_cast<size_t>(y) * width * channels; }

  const uint8_t* row(int y) const { return data.data() + static_cast<size_t>(y) * width * channels; }

  uint8_t at(int x, int y, int c = 0) const { return row(y)[x * channels + c]; }
};

struct PlaneF {
  int width = 0;
  int height = 0;
  std::vector<float> data;

  PlaneF() = default;

  PlaneF(int w, int h, float fill = 0.f) : width(w), height(h), data(static_cast<size_t>(w) * h, fill) {}

  float& at(int x, int y) { return data[static_cast<size_t>(y) * width + x]; }

  float at(int x, int y) const { return data[static_cast<size_t>(y) * width + x]; }
};

/**
 * cv::warpAffine(src, M, (dstWidth, dstHeight), INTER_LINEAR, BORDER_CONSTANT, border) with
 * \p dstToSrc the inverse map (dst pixel -> src pixel: x' = m[0]x + m[1]y + m[2],
 * y' = m[3]x + m[4]y + m[5]). The result is RGB (3 channels) whatever \p src's format.
 */
Plane8 warpAffineRgb(const QImage& src, const double dstToSrc[6], int dstWidth, int dstHeight, uint8_t border);

/** Pixels outside the convex polygon \p poly (points as x0,y0,x1,y1,... in pixels) set to \p value. */
void fillOutsideConvexPolygon(Plane8& img, const std::vector<double>& poly, uint8_t value);

/** cv::resize(src, (w, h), INTER_AREA). */
Plane8 resizeArea(const Plane8& src, int w, int h);

/** RGB -> grey with libpng's coefficients (cv::imread(png, IMREAD_GRAYSCALE)). */
Plane8 rgbToGray(const Plane8& rgb);

/** 1-D Gaussian kernel as cv::getGaussianKernel(n, sigma). */
std::vector<double> gaussianKernel(int n, double sigma);

/** cv::GaussianBlur(src, (0, 0), sigma) on an 8-bit grey image (kernel size as OpenCV for 8U). */
Plane8 gaussianBlur(const Plane8& src, double sigma);

/** cv::GaussianBlur(src, (0, 0), sigma) on a float image (kernel size as OpenCV for 32F). */
PlaneF gaussianBlur(const PlaneF& src, double sigma);

/** numpy.percentile(values, q) (linear interpolation). \p values is reordered. */
double percentile(std::vector<float>& values, double q);
}  // namespace layout
}  // namespace select_content
#endif  // SCANTAILOR_SELECT_CONTENT_LAYOUTIMAGE_H_
