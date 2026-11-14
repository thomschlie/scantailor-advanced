// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_batch_qt.h"

#include <BatchProcessor.h>
#include <DefaultParams.h>
#include <DefaultParamsProfileManager.h>
#include <DefaultParamsProvider.h>
#include <PageInfo.h>
#include <PageSequence.h>
#include <ProjectPages.h>
#include <StageSequence.h>

#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QtTest/QtTest>
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
  const QImage result(output);
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
