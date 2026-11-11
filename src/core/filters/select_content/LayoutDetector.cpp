// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "LayoutDetector.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "ImageTransformation.h"

#ifdef HAVE_LAYOUT_DETECTOR
#include <onnxruntime_cxx_api.h>
#endif

namespace select_content {
namespace {
QMutex g_instanceMutex;
std::shared_ptr<LayoutDetector> g_instance;
bool g_envChecked = false;

QJsonObject rectJson(const QRectF& r) {
  QJsonObject o;
  o.insert("x", r.x());
  o.insert("y", r.y());
  o.insert("width", r.width());
  o.insert("height", r.height());
  return o;
}

QRectF toRect(const layout::Box& b) {
  return QRectF(QPointF(b.x0, b.y0), QPointF(b.x1, b.y1));
}

double round4(double v) {
  return std::round(v * 1e4) / 1e4;
}
}  // namespace

bool LayoutResult::matches(const ImageTransformation& xf,
                           const QRectF& page,
                           const QRectF& content,
                           const int mode) const {
  return xform == xf.transform() && cropArea == xf.resultingPreCropArea() && pageRect == page
         && contentRect == content && contentMode == mode;
}

QJsonObject LayoutResult::toJson() const {
  QJsonObject o;
  o.insert("paper", rectJson(paper));
  if (autoBox.isValid()) {
    o.insert("autoBox", rectJson(autoBox));
  }
  QJsonArray els;
  for (const layout::Element& e : elements) {
    QJsonObject j;
    j.insert("type", e.type);
    j.insert("raw", e.raw);
    j.insert("confidence", round4(e.score));
    j.insert("rect", rectJson(toRect(e.box)));
    j.insert("modelRect", rectJson(toRect(e.modelBox)));
    els.append(j);
  }
  o.insert("elements", els);
  QJsonArray dets;
  for (const layout::Detection& d : detections) {
    QJsonObject j;
    j.insert("raw", d.raw);
    if (!d.cls.isEmpty()) {
      j.insert("class", d.cls);
    }
    if (!d.clsRule.isEmpty()) {
      j.insert("classRule", d.clsRule);
    }
    j.insert("confidence", round4(d.score));
    j.insert("rect", rectJson(toRect(d.box)));
    dets.append(j);
  }
  o.insert("detections", dets);
  o.insert("seconds", std::round(seconds * 1000) / 1000);
  return o;
}

#ifdef HAVE_LAYOUT_DETECTOR
struct LayoutDetector::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "scantailor-layout"};
  std::unique_ptr<Ort::Session> session;
  std::string inputName;
  std::vector<std::string> outputNames;  // logits, boxes
  int inputWidth = 640;
  int inputHeight = 640;
  std::vector<QString> labels;
  mutable QMutex runMutex;
};
#else
struct LayoutDetector::Impl {};
#endif

LayoutDetector::LayoutDetector() : m_impl(std::make_unique<Impl>()) {}

LayoutDetector::~LayoutDetector() = default;

bool LayoutDetector::isBuilt() {
#ifdef HAVE_LAYOUT_DETECTOR
  return true;
#else
  return false;
#endif
}

QString LayoutDetector::configure(const QString& modelPath, const QString& expectedSha256) {
#ifndef HAVE_LAYOUT_DETECTOR
  Q_UNUSED(modelPath);
  Q_UNUSED(expectedSha256);
  return tr("this build has no layout detector (built without ONNX Runtime, ENABLE_LAYOUT_DETECTOR)");
#else
  QFile file(modelPath);
  if (!file.open(QIODevice::ReadOnly)) {
    return tr("cannot read the layout model %1").arg(modelPath);
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    return tr("cannot read the layout model %1").arg(modelPath);
  }
  file.close();
  const QString sha = QString::fromLatin1(hash.result().toHex());
  if (!expectedSha256.isEmpty() && sha.compare(expectedSha256.trimmed(), Qt::CaseInsensitive) != 0) {
    return tr("layout model %1 has sha256 %2, expected %3").arg(modelPath, sha, expectedSha256.trimmed());
  }

  std::shared_ptr<LayoutDetector> det(new LayoutDetector());
  det->m_path = modelPath;
  det->m_sha256 = sha;
  Impl& impl = *det->m_impl;
  try {
    Ort::SessionOptions opt;
    opt.SetIntraOpNumThreads(kIntraOpThreads);
    opt.SetInterOpNumThreads(1);
    opt.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    impl.session = std::make_unique<Ort::Session>(impl.env, modelPath.toStdString().c_str(), opt);
    Ort::AllocatorWithDefaultOptions alloc;
    if (impl.session->GetInputCount() != 1 || impl.session->GetOutputCount() < 2) {
      return tr("layout model %1: expected 1 input and 2 outputs (logits, boxes)").arg(modelPath);
    }
    impl.inputName = impl.session->GetInputNameAllocated(0, alloc).get();
    for (size_t i = 0; i < 2; ++i) {
      impl.outputNames.emplace_back(impl.session->GetOutputNameAllocated(i, alloc).get());
    }
    const std::vector<int64_t> inShape
        = impl.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (inShape.size() == 4 && inShape[2] > 0 && inShape[3] > 0) {
      impl.inputHeight = static_cast<int>(inShape[2]);
      impl.inputWidth = static_cast<int>(inShape[3]);
    }
    const std::vector<int64_t> logitShape
        = impl.session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    const int numClasses = logitShape.size() == 3 ? static_cast<int>(logitShape[2]) : -1;
    // Class names: model metadata "labels" (comma separated), else the fine-tuned Heron's.
    const Ort::ModelMetadata meta = impl.session->GetModelMetadata();
    const Ort::AllocatedStringPtr labels = meta.LookupCustomMetadataMapAllocated("labels", alloc);
    if (labels) {
      for (const QString& l : QString::fromUtf8(labels.get()).split(',')) {
        impl.labels.push_back(l.trimmed());
      }
    } else if (numClasses == static_cast<int>(layout::defaultLabels().size())) {
      impl.labels = layout::defaultLabels();
    } else {
      return tr("layout model %1: %2 classes and no \"labels\" metadata").arg(modelPath).arg(numClasses);
    }
  } catch (const std::exception& e) {
    return tr("cannot load the layout model %1: %2").arg(modelPath, QString::fromLocal8Bit(e.what()));
  }

  const QMutexLocker locker(&g_instanceMutex);
  g_instance = det;
  g_envChecked = true;
  return QString();
#endif
}

std::shared_ptr<LayoutDetector> LayoutDetector::instance() {
  {
    const QMutexLocker locker(&g_instanceMutex);
    if (g_instance || g_envChecked) {
      return g_instance;
    }
    g_envChecked = true;
  }
  const QString path = qEnvironmentVariable("SCANTAILOR_LAYOUT_MODEL");
  if (path.isEmpty()) {
    return nullptr;
  }
  const QString error = configure(path, qEnvironmentVariable("SCANTAILOR_LAYOUT_MODEL_SHA256"));
  if (!error.isEmpty()) {
    std::cerr << "scantailor: layout detector off: " << error.toStdString() << std::endl;
    return nullptr;
  }
  const QMutexLocker locker(&g_instanceMutex);
  return g_instance;
}

std::shared_ptr<LayoutResult> LayoutDetector::analyze(const QImage& image,
                                                      const ImageTransformation& xform,
                                                      const QRectF& pageRect,
                                                      const QRectF& autoBox) const {
#ifndef HAVE_LAYOUT_DETECTOR
  Q_UNUSED(image);
  Q_UNUSED(xform);
  Q_UNUSED(pageRect);
  Q_UNUSED(autoBox);
  throw std::runtime_error("no layout detector in this build");
#else
  using namespace layout;
  QElapsedTimer timer;
  timer.start();
  const Impl& impl = *m_impl;

  // The page at kRenderDpi, as ml/render_pages.py renders it (frame point = pixel / r).
  double frameDpi = xform.preScaledDpi().horizontal();
  if (frameDpi <= 0) {
    frameDpi = 600;
  }
  const double r = kRenderDpi / frameDpi;
  const int w = static_cast<int>(std::nearbyint((pageRect.x() + pageRect.width()) * r));
  const int h = static_cast<int>(std::nearbyint((pageRect.y() + pageRect.height()) * r));
  if (w < 16 || h < 16) {
    throw std::runtime_error("layout detector: page too small");
  }
  const QTransform& back = xform.transformBack();
  const double m[6] = {back.m11() / r, back.m21() / r, back.dx(), back.m12() / r, back.m22() / r, back.dy()};
  Plane8 rgb = warpAffineRgb(image, m, w, h, 255);
  std::vector<double> poly;
  for (const QPointF& p : xform.resultingPreCropArea()) {
    poly.push_back(p.x() * r);
    poly.push_back(p.y() * r);
  }
  fillOutsideConvexPolygon(rgb, poly, 255);
  const Plane8 gray = rgbToGray(rgb);

  // Model input (ml/ft_common.load_page + ml/ft_heron.to_tensor): long side kModelLongSide,
  // then the model's input size, RGB / 255, CHW.
  const double s = static_cast<double>(kModelLongSide) / std::max(w, h);
  const int w1 = static_cast<int>(std::nearbyint(w * s));
  const int h1 = static_cast<int>(std::nearbyint(h * s));
  const Plane8 scaled = resizeArea(rgb, w1, h1);
  const Plane8 input = resizeArea(scaled, impl.inputWidth, impl.inputHeight);
  const size_t plane = static_cast<size_t>(impl.inputWidth) * impl.inputHeight;
  std::vector<float> tensor(3 * plane);
  for (size_t i = 0; i < plane; ++i) {
    for (int c = 0; c < 3; ++c) {
      tensor[c * plane + i] = static_cast<float>(input.data[i * 3 + c]) / 255.f;
    }
  }

  std::vector<Detection> dets;
  {
    const QMutexLocker locker(&impl.runMutex);
    const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 4> shape{1, 3, impl.inputHeight, impl.inputWidth};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, tensor.data(), tensor.size(), shape.data(), shape.size());
    const char* inNames[] = {impl.inputName.c_str()};
    const char* outNames[] = {impl.outputNames[0].c_str(), impl.outputNames[1].c_str()};
    std::vector<Ort::Value> out = impl.session->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 2);
    const std::vector<int64_t> ls = out[0].GetTensorTypeAndShapeInfo().GetShape();
    const std::vector<int64_t> bs = out[1].GetTensorTypeAndShapeInfo().GetShape();
    if (ls.size() != 3 || bs.size() != 3 || ls[1] != bs[1] || bs[2] != 4) {
      throw std::runtime_error("layout model: unexpected output shapes");
    }
    dets = decodeDetections(out[0].GetTensorData<float>(), out[1].GetTensorData<float>(), static_cast<int>(ls[1]),
                            static_cast<int>(ls[2]), impl.labels, w1, h1, kScoreThreshold, kNmsIou);
  }

  auto result = std::make_shared<LayoutResult>();
  const Box page{pageRect.left(), pageRect.top(), pageRect.right(), pageRect.bottom()};
  std::optional<Box> autoB;
  if (autoBox.isValid()) {
    autoB = Box{autoBox.left(), autoBox.top(), autoBox.right(), autoBox.bottom()};
  }
  // Model image px -> frame px; class mapping and side-strip rule (ml/ft_common.element).
  const double toFrame = 1.0 / (s * r);
  std::vector<Detection> rendered;
  for (Detection& d : dets) {
    d.box = d.box.scaled(toFrame);
    d.cls = mapClass(d.raw, d.box, frameDpi);
    d.clsRule = tabRule(d.box, page, autoB, frameDpi) ? QStringLiteral("tab") : d.cls;
    if (d.raw == QLatin1String("text")) {  // the fine-tuned "text" class (negatives) is never ours
      d.cls.clear();
      d.clsRule.clear();
    }
    Detection dr = d;
    dr.box = d.box.scaled(r);
    rendered.push_back(dr);
  }
  const Box paper = paperRect(gray, kRenderDpi);
  std::vector<Element> elements = selectElements(rendered, gray, kRenderDpi, paper);
  for (Element& e : elements) {
    e.box = e.box.scaled(1.0 / r);
    e.modelBox = e.modelBox.scaled(1.0 / r);
  }

  result->xform = xform.transform();
  result->cropArea = xform.resultingPreCropArea();
  result->autoBox = autoBox;
  result->paper = toRect(paper.scaled(1.0 / r));
  result->detections = std::move(dets);
  result->elements = std::move(elements);
  result->seconds = timer.elapsed() / 1000.0;
  return result;
#endif
}
}  // namespace select_content
