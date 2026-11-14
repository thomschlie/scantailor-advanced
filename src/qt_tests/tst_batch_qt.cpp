// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_batch_qt.h"

#include <BatchProcessor.h>
#include <DefaultParams.h>
#include <DefaultParamsProfileManager.h>
#include <DefaultParamsProvider.h>
#include <ImageLoader.h>
#include <PageInfo.h>
#include <PageSequence.h>
#include <ProjectPages.h>
#include <StageSequence.h>
#include <filters/page_layout/Filter.h>
#include <filters/page_layout/Params.h>
#include <filters/page_layout/Settings.h>
#include <filters/page_layout/Utils.h>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QtTest/QtTest>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
// The page's text lines are tilted by this much (degrees, clockwise on screen).
const double kTilt = 2.0;
const int kDpi = 300;

// A page of "text" at 300 dpi: lines of word-sized black boxes, drawn without antialiasing
// (deterministic, no fonts involved), rotated by kTilt about the page centre.
QImage makeTiltedTextPage() {
  QImage image(1500, 2100, QImage::Format_RGB32);
  image.fill(Qt::white);
  const int dpm = static_cast<int>(std::lround(kDpi / 0.0254));
  image.setDotsPerMeterX(dpm);
  image.setDotsPerMeterY(dpm);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.translate(image.width() / 2.0, image.height() / 2.0);
  painter.rotate(kTilt);
  painter.translate(-image.width() / 2.0, -image.height() / 2.0);
  painter.setPen(Qt::NoPen);
  painter.setBrush(Qt::black);

  uint32_t state = 12345;  // own generator: the same page on every platform
  auto next = [&state](const int range) {
    state = state * 1103515245u + 12345u;
    return static_cast<int>((state >> 16) % static_cast<uint32_t>(range));
  };
  const int left = 220, right = 1280, top = 260, lineHeight = 52;
  for (int line = 0; line < 30; ++line) {
    const int y = top + line * lineHeight;
    const int lineEnd = (line % 8 == 7) ? left + 300 + next(400) : right;  // short last line of a paragraph
    for (int x = left + ((line % 8 == 0) ? 60 : 0); x < lineEnd;) {
      const int width = std::min(25 + next(140), lineEnd - x);
      painter.drawRect(x, y, width, 24);
      x += width + 18;
    }
  }
  painter.end();
  return image;
}

BatchProcessor::Options allStages(const BatchProcessor& batch) {
  BatchProcessor::Options options;
  options.startFilter = 0;
  options.endFilter = batch.numStages() - 1;
  options.threads = 2;
  return options;
}

PageInfo onlyPage(const BatchProcessor& batch) {
  const PageSequence pages = batch.pages()->toPageSequence(PAGE_VIEW);
  return pages.numPages() == 1 ? pages.pageAt(static_cast<size_t>(0)) : PageInfo();
}

// The deskew <params> element of the only page in a saved project; the settings of the
// filters are private to them, but the project file shows them as the GUI saves them.
QDomElement deskewParams(QDomDocument& doc, const QString& projectFile) {
  QFile file(projectFile);
  if (!file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) {
    return QDomElement();
  }
  return doc.documentElement()
      .firstChildElement("filters")
      .firstChildElement("deskew")
      .firstChildElement("page")
      .firstChildElement("params");
}

bool writeDocument(const QDomDocument& doc, const QString& path) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && (file.write(doc.toByteArray()) > 0);
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Copies of the test page, one file per page.
QStringList pageCopies(const QString& image, const QString& dir, const int count) {
  QStringList files;
  for (int i = 0; i < count; ++i) {
    const QString file = QDir(dir).filePath(QString("copy%1.png").arg(i));
    QFile::remove(file);
    if (!QFile::copy(image, file)) {
      return {};
    }
    files.append(file);
  }
  return files;
}

page_layout::Params pageLayoutParams(const BatchProcessor& batch, const PageId& pageId) {
  const std::unique_ptr<page_layout::Params> params
      = batch.stages()->pageLayoutFilter()->settings()->getPageParams(pageId);
  return params ? *params
                : page_layout::Params(Margins(), QRectF(), QRectF(), QSizeF(), page_layout::Alignment(), true);
}

bool sameMargins(const Margins& a, const Margins& b) {
  return a.left() == b.left() && a.top() == b.top() && a.right() == b.right() && a.bottom() == b.bottom();
}

// Ink (dark pixels) bounding box of an output image.
QRect inkBox(const QImage& image) {
  const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
  int left = gray.width(), right = -1, top = gray.height(), bottom = -1;
  for (int y = 0; y < gray.height(); ++y) {
    const uchar* line = gray.constScanLine(y);
    for (int x = 0; x < gray.width(); ++x) {
      if (line[x] < 128) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  return (right < 0) ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

page_layout::Alignment outerBottom(const BatchProcessor::PageSide side) {
  return page_layout::Alignment(page_layout::Alignment::BOTTOM, (side == BatchProcessor::PageSide::LEFT)
                                                                    ? page_layout::Alignment::LEFT
                                                                    : page_layout::Alignment::RIGHT);
}
}  // namespace

void TestBatchQt::initTestCase() {
  QVERIFY(m_dir.isValid());
  m_image = m_dir.filePath("page.png");
  QVERIFY(makeTiltedTextPage().save(m_image));
  // The built-in default parameters, whatever profile the user's settings select.
  DefaultParamsProvider::getInstance().setParams(DefaultParamsProfileManager(m_dir.path()).createDefaultProfile(),
                                                 "Default");
}

void TestBatchQt::testAllStagesProduceOutput() {
  BatchProcessor batch({m_image}, m_dir.filePath("all"), Qt::LeftToRight, 0);
  QCOMPARE(batch.process(allStages(batch)), 0);

  const PageInfo page = onlyPage(batch);
  QVERIFY(!page.isNull());
  const QString output = batch.outputFileNameGenerator().filePathFor(page.id());
  QVERIFY(QFileInfo::exists(output));
  QVERIFY(QFileInfo(output).size() > 10000);
  // ScanTailor reads its TIFF output through libtiff; QImage would need the qtiff plugin.
  const QImage result = ImageLoader::load(output);
  QVERIFY(!result.isNull());
  // Content box plus the default margins, at the default output resolution: far from empty,
  // not larger than the page.
  QVERIFY(result.width() > 1000);
  QVERIFY(result.height() > 1000);

  // Deskew undoes the tilt (a counter-clockwise rotation).
  const QString project = m_dir.filePath("all.ScanTailor");
  QVERIFY(batch.saveProject(project));
  QDomDocument doc;
  const QDomElement params = deskewParams(doc, project);
  QVERIFY(!params.isNull());
  QCOMPARE(params.attribute("mode"), QString("auto"));
  const double angle = params.attribute("angle").toDouble();
  QVERIFY2(std::abs(angle + kTilt) < 0.1, qPrintable(QString::number(angle)));
}

void TestBatchQt::testManualSettingSurvives() {
  const QString project = m_dir.filePath("manual.ScanTailor");
  {
    BatchProcessor batch({m_image}, m_dir.filePath("manual"), Qt::LeftToRight, 0);
    QCOMPARE(batch.process(allStages(batch)), 0);
    QVERIFY(batch.saveProject(project));
  }
  // A deskew angle set by hand, as the GUI stores it.
  {
    QDomDocument doc;
    QDomElement params = deskewParams(doc, project);
    QVERIFY(!params.isNull());
    params.setAttribute("mode", "manual");
    params.setAttribute("angle", "0.5");
    QVERIFY(writeDocument(doc, project));
  }
  // The project processed again from "Deskew" on keeps it.
  BatchProcessor batch(project, QString());
  BatchProcessor::Options options = allStages(batch);
  options.startFilter = batch.stages()->deskewFilterIdx();
  QCOMPARE(batch.process(options), 0);
  QVERIFY(batch.saveProject(project));
  QDomDocument doc;
  const QDomElement params = deskewParams(doc, project);
  QVERIFY(!params.isNull());
  QCOMPARE(params.attribute("mode"), QString("manual"));
  QCOMPARE(params.attribute("angle").toDouble(), 0.5);
}

void TestBatchQt::testTwoRunsAreIdentical() {
  QString outputs[2];
  for (int run = 0; run < 2; ++run) {
    BatchProcessor batch({m_image}, m_dir.filePath(QString("run%1").arg(run)), Qt::LeftToRight, 0);
    QCOMPARE(batch.process(allStages(batch)), 0);
    outputs[run] = batch.outputFileNameGenerator().filePathFor(onlyPage(batch).id());
  }
  QVERIFY(outputs[0] != outputs[1]);
  const QByteArray first = readFile(outputs[0]);
  QVERIFY(!first.isEmpty());
  QVERIFY(first == readFile(outputs[1]));
}

void TestBatchQt::testOuterBottomLayout() {
  using page_layout::Alignment;
  QDir().mkpath(m_dir.filePath("outer-in"));
  const QStringList images = pageCopies(m_image, m_dir.filePath("outer-in"), 3);
  QCOMPARE(images.size(), 3);
  BatchProcessor batch(images, m_dir.filePath("outer"), Qt::LeftToRight, 0);
  // The third page has page layout settings of its own (as set by hand): larger margins, so
  // the other two pages are smaller than the aggregate size and the alignment shows.
  const PageId manual = batch.pages()->toPageSequence(PAGE_VIEW).pageAt(static_cast<size_t>(2)).id();
  const page_layout::Params manualParams(Margins(6, 6, 6, 6), QRectF(), QRectF(), QSizeF(),
                                         Alignment(Alignment::TOP, Alignment::HCENTER), false);
  batch.stages()->pageLayoutFilter()->settings()->setPageParams(manual, manualParams);

  BatchProcessor::Options options = allStages(batch);
  options.pageLayoutMarginsMm = Margins(0, 0, 0, 11);
  options.alignOuterBottom = true;
  QCOMPARE(batch.process(options), 0);

  const PageSequence pages = batch.pages()->toPageSequence(PAGE_VIEW);
  QCOMPARE(static_cast<int>(pages.numPages()), 3);
  QCOMPARE(pages.pageAt(static_cast<size_t>(2)).id(), manual);
  // Single pages: the first is a recto (right page), then they alternate.
  const Alignment::Horizontal expected[2] = {Alignment::RIGHT, Alignment::LEFT};
  for (int i = 0; i < 2; ++i) {
    const PageId id = pages.pageAt(static_cast<size_t>(i)).id();
    const page_layout::Params params = pageLayoutParams(batch, id);
    QCOMPARE(params.alignment(), Alignment(Alignment::BOTTOM, expected[i]));
    QVERIFY(sameMargins(params.hardMarginsMM(), Margins(0, 0, 0, 11)));
    QVERIFY(!params.isAutoMarginsEnabled());

    // In the output the text sits at the outer edge, 11 mm above the bottom edge.
    const QImage out = ImageLoader::load(batch.outputFileNameGenerator().filePathFor(id));
    QVERIFY(!out.isNull());
    QVERIFY(out.dotsPerMeterX() > 0);
    const double pxPerMm = out.dotsPerMeterX() / 1000.0;
    const QRect ink = inkBox(out);
    QVERIFY(!ink.isNull());
    const double outerMm = (expected[i] == Alignment::LEFT ? ink.left() : out.width() - 1 - ink.right()) / pxPerMm;
    const double innerMm = (expected[i] == Alignment::LEFT ? out.width() - 1 - ink.right() : ink.left()) / pxPerMm;
    const double bottomMm = (out.height() - 1 - ink.bottom()) / pxPerMm;
    QVERIFY2(outerMm < 1.0, qPrintable(QString("page %1: outer margin %2 mm").arg(i).arg(outerMm)));
    QVERIFY2(innerMm > 10.0, qPrintable(QString("page %1: inner margin %2 mm").arg(i).arg(innerMm)));
    QVERIFY2(std::abs(bottomMm - 11.0) < 1.0, qPrintable(QString("page %1: bottom margin %2 mm").arg(i).arg(bottomMm)));
  }
  // The page with settings of its own keeps them.
  const page_layout::Params kept = pageLayoutParams(batch, manual);
  QCOMPARE(kept.alignment(), manualParams.alignment());
  QVERIFY(sameMargins(kept.hardMarginsMM(), manualParams.hardMarginsMM()));
}

void TestBatchQt::testOuterBottomRightToLeft() {
  using page_layout::Alignment;
  QDir().mkpath(m_dir.filePath("rtl-in"));
  const QStringList images = pageCopies(m_image, m_dir.filePath("rtl-in"), 2);
  QCOMPARE(images.size(), 2);
  BatchProcessor batch(images, m_dir.filePath("rtl"), Qt::RightToLeft, 0);
  BatchProcessor::Options options = allStages(batch);
  options.endFilter = batch.stages()->pageLayoutFilterIdx();
  options.alignOuterBottom = true;
  QCOMPARE(batch.process(options), 0);
  const PageSequence pages = batch.pages()->toPageSequence(PAGE_VIEW);
  QCOMPARE(static_cast<int>(pages.numPages()), 2);
  // Right to left: the first page (a recto) is on the left.
  QCOMPARE(pageLayoutParams(batch, pages.pageAt(static_cast<size_t>(0)).id()).alignment(),
           Alignment(Alignment::BOTTOM, Alignment::LEFT));
  QCOMPARE(pageLayoutParams(batch, pages.pageAt(static_cast<size_t>(1)).id()).alignment(),
           Alignment(Alignment::BOTTOM, Alignment::RIGHT));
  // Without --margins-mm the profile's margins stay.
  QVERIFY(sameMargins(pageLayoutParams(batch, pages.pageAt(static_cast<size_t>(0)).id()).hardMarginsMM(),
                      Margins(10, 5, 10, 5)));
}

void TestBatchQt::testFirstPageAndPageSide() {
  using Side = BatchProcessor::PageSide;
  QDir().mkpath(m_dir.filePath("sides-in"));
  const QStringList images = pageCopies(m_image, m_dir.filePath("sides-in"), 4);
  QCOMPARE(images.size(), 4);
  BatchProcessor batch(images, m_dir.filePath("sides"), Qt::LeftToRight, 0);
  BatchProcessor::Options options = allStages(batch);
  options.endFilter = batch.stages()->pageLayoutFilterIdx();
  options.alignOuterBottom = true;

  // An image that is not in the project: an error before any stage runs.
  options.pageSideAnchors = {{"missing.png", Side::LEFT}};
  bool threw = false;
  try {
    batch.process(options);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  QVERIFY(threw);
  const PageSequence pages = batch.pages()->toPageSequence(PAGE_VIEW);
  QCOMPARE(static_cast<int>(pages.numPages()), 4);
  QVERIFY(batch.stages()->pageLayoutFilter()->settings()->isParamsNull(pages.pageAt(static_cast<size_t>(0)).id()));

  // The first page on the left (a book that starts with a verso); the third page anchored by
  // its file name, the fourth by its path: left again, as after a missing scan.
  options.firstPageSide = Side::LEFT;
  options.pageSideAnchors = {{"copy2.png", Side::LEFT}, {images[3], Side::LEFT}};
  std::vector<std::optional<Side>> reported;
  options.pageLayoutDecided
      = [&reported](const PageInfo&, const BatchProcessor::PageLayoutOutcome outcome, const std::optional<Side> side) {
          QCOMPARE(outcome, BatchProcessor::PageLayoutOutcome::SET);
          reported.push_back(side);
        };
  QCOMPARE(batch.process(options), 0);
  const Side expected[4] = {Side::LEFT, Side::RIGHT, Side::LEFT, Side::LEFT};
  QCOMPARE(static_cast<int>(reported.size()), 4);
  for (int i = 0; i < 4; ++i) {
    QCOMPARE(pageLayoutParams(batch, pages.pageAt(static_cast<size_t>(i)).id()).alignment(), outerBottom(expected[i]));
    QVERIFY(reported[i] && *reported[i] == expected[i]);
  }
}

void TestBatchQt::testResetLayout() {
  using page_layout::Alignment;
  using Side = BatchProcessor::PageSide;
  using Outcome = BatchProcessor::PageLayoutOutcome;
  QDir().mkpath(m_dir.filePath("reset-in"));
  const QStringList images = pageCopies(m_image, m_dir.filePath("reset-in"), 5);
  QCOMPARE(images.size(), 5);
  BatchProcessor batch(images, m_dir.filePath("reset"), Qt::LeftToRight, 0);
  const std::shared_ptr<page_layout::Settings> settings = batch.stages()->pageLayoutFilter()->settings();
  const PageSequence pages = batch.pages()->toPageSequence(PAGE_VIEW);
  QCOMPARE(static_cast<int>(pages.numPages()), 5);
  const auto id = [&pages](const int i) { return pages.pageAt(static_cast<size_t>(i)).id(); };

  // A processed project: outer + bottom, first page on the right.
  BatchProcessor::Options first = allStages(batch);
  first.endFilter = batch.stages()->pageLayoutFilterIdx();
  first.alignOuterBottom = true;
  first.pageLayoutMarginsMm = Margins(0, 0, 0, 11);
  QCOMPARE(batch.process(first), 0);
  QCOMPARE(pageLayoutParams(batch, id(0)).alignment(), outerBottom(Side::RIGHT));
  // Page 1 back at the profile's defaults (automatic too); page 2 aligned by hand (top +
  // centre), page 3 with margins of its own: both set by hand.
  const page_layout::Params stored2 = pageLayoutParams(batch, id(2));
  const page_layout::Params stored3 = pageLayoutParams(batch, id(3));
  const page_layout::Params defaults
      = page_layout::Utils::buildDefaultParams(pages.pageAt(static_cast<size_t>(1)).metadata().dpi());
  settings->setPageParams(id(1), defaults);
  const page_layout::Params byHand2(stored2.hardMarginsMM(), stored2.pageRect(), stored2.contentRect(),
                                    stored2.contentSizeMM(), Alignment(Alignment::TOP, Alignment::HCENTER), false);
  const page_layout::Params byHand3(Margins(3, 3, 3, 3), stored3.pageRect(), stored3.contentRect(),
                                    stored3.contentSizeMM(), stored3.alignment(), false);
  settings->setPageParams(id(2), byHand2);
  settings->setPageParams(id(3), byHand3);

  // The book starts with a verso after all: without a reset nothing changes.
  BatchProcessor::Options again = first;
  again.startFilter = again.endFilter;
  again.firstPageSide = Side::LEFT;
  std::vector<Outcome> outcomes;
  again.pageLayoutDecided
      = [&outcomes](const PageInfo&, const Outcome outcome, std::optional<Side>) { outcomes.push_back(outcome); };
  QCOMPARE(batch.process(again), 0);
  QCOMPARE(pageLayoutParams(batch, id(0)).alignment(), outerBottom(Side::RIGHT));
  QCOMPARE(pageLayoutParams(batch, id(1)).alignment(), defaults.alignment());
  QVERIFY(std::all_of(outcomes.begin(), outcomes.end(), [](Outcome o) { return o == Outcome::KEPT; }));

  // --reset-layout: the automatic pages (mirrored ones and the one at the profile's defaults)
  // get the options; the two set by hand are spared. Rectangles stay as stored.
  outcomes.clear();
  const QRectF contentRect0 = pageLayoutParams(batch, id(0)).contentRect();
  again.resetLayout = BatchProcessor::ResetLayout::AUTOMATIC;
  QCOMPARE(batch.process(again), 0);
  const std::vector<Outcome> expectedOutcomes
      = {Outcome::SET, Outcome::SET, Outcome::SPARED, Outcome::SPARED, Outcome::SET};
  QVERIFY(outcomes == expectedOutcomes);
  const Side expected[5] = {Side::LEFT, Side::RIGHT, Side::LEFT, Side::RIGHT, Side::LEFT};
  for (const int i : {0, 1, 4}) {
    const page_layout::Params params = pageLayoutParams(batch, id(i));
    QCOMPARE(params.alignment(), outerBottom(expected[i]));
    QVERIFY(sameMargins(params.hardMarginsMM(), Margins(0, 0, 0, 11)));
    QVERIFY(!params.isAutoMarginsEnabled());
  }
  QCOMPARE(pageLayoutParams(batch, id(0)).contentRect(), contentRect0);
  QCOMPARE(pageLayoutParams(batch, id(2)).alignment(), byHand2.alignment());
  QVERIFY(sameMargins(pageLayoutParams(batch, id(3)).hardMarginsMM(), byHand3.hardMarginsMM()));

  // --reset-layout=all: every page.
  outcomes.clear();
  again.resetLayout = BatchProcessor::ResetLayout::ALL;
  QCOMPARE(batch.process(again), 0);
  QVERIFY(std::all_of(outcomes.begin(), outcomes.end(), [](Outcome o) { return o == Outcome::SET; }));
  for (int i = 0; i < 5; ++i) {
    const page_layout::Params params = pageLayoutParams(batch, id(i));
    QCOMPARE(params.alignment(), outerBottom(expected[i]));
    QVERIFY(sameMargins(params.hardMarginsMM(), Margins(0, 0, 0, 11)));
  }
}
