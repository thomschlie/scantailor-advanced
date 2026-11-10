// Copyright (C) 2026  scantailor-cli contributors
// Based on ConsoleBatch of Scan Tailor 0.9.x (Joseph Artsimovich, Petr Kovar), GPLv3.
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "ConsoleBatch.h"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRunnable>
#include <QTemporaryFile>
#include <QThreadPool>
#include <atomic>
#include <iostream>
#include <map>
#include <stdexcept>

#include "CompositeCacheDrivenTask.h"
#include "ContentBoxPropagator.h"
#include "Dpi.h"
#include "PageOrientationPropagator.h"
#include "filters/deskew/CacheDrivenTask.h"
#include "filters/fix_orientation/CacheDrivenTask.h"
#include "filters/output/CacheDrivenTask.h"
#include "filters/page_layout/CacheDrivenTask.h"
#include "filters/page_split/CacheDrivenTask.h"
#include "filters/select_content/CacheDrivenTask.h"
#include "FileNameDisambiguator.h"
#include "ImageFileInfo.h"
#include "ImageMetadata.h"
#include "ImageMetadataLoader.h"
#include "LoadFileTask.h"
#include "PageRange.h"
#include "PageSelectionAccessor.h"
#include "PageSelectionProvider.h"
#include "PageSequence.h"
#include "ProjectPages.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "SelectedPage.h"
#include "StageSequence.h"
#include "ThumbnailPixmapCache.h"
#include <core/Utils.h>
#include "filters/deskew/Task.h"
#include "filters/fix_orientation/Task.h"
#include "filters/output/Task.h"
#include "filters/page_layout/Task.h"
#include "filters/page_split/Task.h"
#include "filters/select_content/Task.h"

namespace {
// The filters ask for the page selection only from GUI option widgets ("apply to ...").
// Headless there is no selection; answer with all pages so nothing dereferences null.
class AllPagesSelectionProvider : public PageSelectionProvider {
 public:
  explicit AllPagesSelectionProvider(std::shared_ptr<ProjectPages> pages) : m_pages(std::move(pages)) {}

  PageSequence allPages() const override { return m_pages->toPageSequence(PAGE_VIEW); }

  std::set<PageId> selectedPages() const override { return allPages().selectAll(); }

  std::vector<PageRange> selectedRanges() const override { return {}; }

 private:
  std::shared_ptr<ProjectPages> m_pages;
};

const char* const kStageNames[] = {"fix_orientation", "page_split",  "deskew",
                                   "select_content",  "page_layout", "output"};
}  // namespace

ConsoleBatch::ConsoleBatch(const QStringList& imageFiles,
                           const QString& outputDir,
                           const Qt::LayoutDirection layoutDirection,
                           const int forceDpi) {
  std::vector<ImageFileInfo> files;
  for (const QString& path : imageFiles) {
    std::vector<ImageMetadata> perPage;
    const ImageMetadataLoader::Status st
        = ImageMetadataLoader::load(path, [&](const ImageMetadata& m) { perPage.push_back(m); });
    if (st != ImageMetadataLoader::LOADED) {
      throw std::runtime_error(("cannot read image: " + path).toStdString());
    }
    for (ImageMetadata& m : perPage) {
      if (forceDpi > 0) {
        m.setDpi(Dpi(forceDpi, forceDpi));
      } else if (!m.isDpiOK()) {
        throw std::runtime_error(("no usable resolution in " + path + " (use --source-dpi)").toStdString());
      }
    }
    files.emplace_back(QFileInfo(path), perPage);
  }
  if (files.empty()) {
    throw std::runtime_error("no input images");
  }
  m_pages = std::make_shared<ProjectPages>(files, ProjectPages::AUTO_PAGES, layoutDirection);
  init(outputDir, std::make_shared<FileNameDisambiguator>());
}

ConsoleBatch::ConsoleBatch(const QString& projectFile, const QString& outputDir) {
  QFile file(projectFile);
  if (!file.open(QIODevice::ReadOnly)) {
    throw std::runtime_error(("cannot open project: " + projectFile).toStdString());
  }
  QDomDocument doc;
  if (!doc.setContent(&file)) {
    throw std::runtime_error(("project file is broken: " + projectFile).toStdString());
  }
  file.close();

  m_reader = std::make_unique<ProjectReader>(doc, QFileInfo(projectFile).absoluteFilePath());
  if (!m_reader->success()) {
    throw std::runtime_error(("cannot load project: " + projectFile).toStdString());
  }
  m_pages = m_reader->pages();
  init(outputDir.isEmpty() ? m_reader->outputDirectory() : outputDir, m_reader->namingDisambiguator());
  m_reader->readFilterSettings(m_stages->filters());
}

ConsoleBatch::~ConsoleBatch() = default;

void ConsoleBatch::init(const QString& outputDir, const std::shared_ptr<FileNameDisambiguator>& disambiguator) {
  if (outputDir.isEmpty()) {
    throw std::runtime_error("no output directory");
  }
  const QString outDir = QDir(outputDir).absolutePath();
  QDir().mkpath(outDir);
  core::Utils::maybeCreateCacheDir(outDir);

  m_outFileNameGen = OutputFileNameGenerator(disambiguator, outDir, m_pages->layoutDirection());
  // Same order as MainWindow::projectOpened: register the source files, then the stages.
  for (const PageInfo& page : m_pages->toPageSequence(IMAGE_VIEW)) {
    m_outFileNameGen.disambiguator()->registerFile(page.imageId().filePath());
  }
  m_stages = std::make_shared<StageSequence>(
      m_pages, PageSelectionAccessor(std::make_shared<AllPagesSelectionProvider>(m_pages)));
  m_thumbnailCache = core::Utils::createThumbnailCache(outDir);
}

int ConsoleBatch::numFilters() const {
  return static_cast<int>(m_stages->filters().size());
}

BackgroundTaskPtr ConsoleBatch::createCompositeTask(const PageInfo& page, const int lastFilterIdx) {
  // Mirrors MainWindow::createCompositeTask with batch = true, debug = false, so the
  // result is the same as a GUI batch run.
  const bool batch = true;
  const bool debug = false;
  std::shared_ptr<fix_orientation::Task> fixOrientationTask;
  std::shared_ptr<page_split::Task> pageSplitTask;
  std::shared_ptr<deskew::Task> deskewTask;
  std::shared_ptr<select_content::Task> selectContentTask;
  std::shared_ptr<page_layout::Task> pageLayoutTask;
  std::shared_ptr<output::Task> outputTask;

  if (lastFilterIdx >= m_stages->outputFilterIdx()) {
    outputTask = m_stages->outputFilter()->createTask(page.id(), m_thumbnailCache, m_outFileNameGen, batch, debug);
  }
  if (lastFilterIdx >= m_stages->pageLayoutFilterIdx()) {
    pageLayoutTask = m_stages->pageLayoutFilter()->createTask(page.id(), outputTask, batch, debug);
  }
  if (lastFilterIdx >= m_stages->selectContentFilterIdx()) {
    selectContentTask = m_stages->selectContentFilter()->createTask(page.id(), pageLayoutTask, batch, debug);
  }
  if (lastFilterIdx >= m_stages->deskewFilterIdx()) {
    deskewTask = m_stages->deskewFilter()->createTask(page.id(), selectContentTask, batch, debug);
  }
  if (lastFilterIdx >= m_stages->pageSplitFilterIdx()) {
    pageSplitTask = m_stages->pageSplitFilter()->createTask(page, deskewTask, batch, debug);
  }
  fixOrientationTask = m_stages->fixOrientationFilter()->createTask(page.id(), pageSplitTask, batch);
  return std::make_shared<LoadFileTask>(BackgroundTask::BATCH, page, m_thumbnailCache, m_pages, fixOrientationTask);
}

std::shared_ptr<CompositeCacheDrivenTask> ConsoleBatch::createCompositeCacheDrivenTask(const int lastFilterIdx) {
  // Mirrors MainWindow::createCompositeCacheDrivenTask (used by the propagators).
  std::shared_ptr<fix_orientation::CacheDrivenTask> fixOrientationTask;
  std::shared_ptr<page_split::CacheDrivenTask> pageSplitTask;
  std::shared_ptr<deskew::CacheDrivenTask> deskewTask;
  std::shared_ptr<select_content::CacheDrivenTask> selectContentTask;
  std::shared_ptr<page_layout::CacheDrivenTask> pageLayoutTask;
  std::shared_ptr<output::CacheDrivenTask> outputTask;

  if (lastFilterIdx >= m_stages->outputFilterIdx()) {
    outputTask = m_stages->outputFilter()->createCacheDrivenTask(m_outFileNameGen);
  }
  if (lastFilterIdx >= m_stages->pageLayoutFilterIdx()) {
    pageLayoutTask = m_stages->pageLayoutFilter()->createCacheDrivenTask(outputTask);
  }
  if (lastFilterIdx >= m_stages->selectContentFilterIdx()) {
    selectContentTask = m_stages->selectContentFilter()->createCacheDrivenTask(pageLayoutTask);
  }
  if (lastFilterIdx >= m_stages->deskewFilterIdx()) {
    deskewTask = m_stages->deskewFilter()->createCacheDrivenTask(selectContentTask);
  }
  if (lastFilterIdx >= m_stages->pageSplitFilterIdx()) {
    pageSplitTask = m_stages->pageSplitFilter()->createCacheDrivenTask(deskewTask);
  }
  fixOrientationTask = m_stages->fixOrientationFilter()->createCacheDrivenTask(pageSplitTask);
  return fixOrientationTask;
}

void ConsoleBatch::enterStage(const int stage, const bool firstStage) {
  // MainWindow::filterSelectionChanged: when the user moves from a stage at or above
  // "Fix orientation" / "Select content" to one below it, the orientations / content
  // boxes are propagated to "Split pages" / "Page layout" first. Do the same when
  // a run starts at, or steps into, such a stage.
  const int fixIdx = m_stages->fixOrientationFilterIdx();
  const int contentIdx = m_stages->selectContentFilterIdx();
  const int prev = firstStage ? -1 : stage - 1;
  if (stage > fixIdx && prev <= fixIdx) {
    PageOrientationPropagator(m_stages->pageSplitFilter(), createCompositeCacheDrivenTask(fixIdx))
        .propagate(*m_pages);
  }
  if (stage > contentIdx && prev <= contentIdx) {
    ContentBoxPropagator(m_stages->pageLayoutFilter(), createCompositeCacheDrivenTask(contentIdx))
        .propagate(*m_pages);
  }
}

int ConsoleBatch::process(const Options& options) {
  const int last = numFilters() - 1;
  if (options.startFilter < 0 || options.startFilter > last || options.endFilter < options.startFilter
      || options.endFilter > last) {
    throw std::runtime_error("stage range out of bounds");
  }

  QThreadPool pool;
  pool.setMaxThreadCount(std::max(1, options.threads));
  std::atomic<int> failed{0};
  QMutex outMutex;

  for (int stage = options.startFilter; stage <= options.endFilter; ++stage) {
    enterStage(stage, stage == options.startFilter);
    // Re-read the page sequence every stage: page_split may change the number of pages.
    const PageSequence pages = m_pages->toPageSequence(PAGE_VIEW);
    // As MainWindow::startBatchProcessing: pages without settings get the default
    // parameters of every stage before their tasks are created (tasks without
    // parameters compute nothing, e.g. no content box).
    for (const PageInfo& page : pages) {
      for (int i = 0; i < numFilters(); ++i) {
        m_stages->filterAt(i)->loadDefaultSettings(page);
      }
    }
    if (options.verbose) {
      std::cout << "stage " << (stage + 1) << " " << kStageNames[stage] << ": " << pages.numPages() << " pages"
                << std::endl;
    }
    for (const PageInfo& page : pages) {
      BackgroundTaskPtr task = createCompositeTask(page, stage);
      const QString path = page.imageId().filePath();
      pool.start(QRunnable::create([task, path, &failed, &outMutex, &options]() {
        try {
          const FilterResultPtr result = (*task)();
          if (!result || !result->filter()) {
            // LoadFileTask returns a result without a filter when the image cannot be loaded.
            ++failed;
            QMutexLocker lock(&outMutex);
            std::cerr << "error: " << path.toStdString() << ": cannot process" << std::endl;
          } else if (options.verbose) {
            QMutexLocker lock(&outMutex);
            std::cout << "  " << path.toStdString() << std::endl;
          }
        } catch (const std::exception& e) {
          ++failed;
          QMutexLocker lock(&outMutex);
          std::cerr << "error: " << path.toStdString() << ": " << e.what() << std::endl;
        }
      }));
    }
    pool.waitForDone();
  }
  return failed.load();
}

bool ConsoleBatch::saveProject(const QString& projectFile) const {
  const PageSequence pages = m_pages->toPageSequence(PAGE_VIEW);
  SelectedPage selected;
  if (m_reader && !m_reader->selectedPage().isNull()) {
    selected = m_reader->selectedPage();
  } else if (pages.numPages() > 0) {
    selected = SelectedPage(pages.pageAt(static_cast<size_t>(0)).id(), PAGE_VIEW);
  }
  ProjectWriter writer(m_pages, selected, m_outFileNameGen);
  return writer.write(projectFile, m_stages->filters());
}

namespace {
QJsonObject attributesOf(const QDomElement& el) {
  QJsonObject obj;
  const QDomNamedNodeMap attrs = el.attributes();
  for (int i = 0; i < attrs.count(); ++i) {
    const QDomAttr a = attrs.item(i).toAttr();
    obj.insert(a.name(), a.value());
  }
  return obj;
}

// Generic dump of one <page> (or <image>) settings element: its child elements with their
// attributes, nested one level (e.g. <params mode="auto"><content-rect .../></params>).
QJsonObject dumpSettings(const QDomElement& pageEl) {
  QJsonObject out;
  for (QDomElement c = pageEl.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
    QJsonObject obj = attributesOf(c);
    for (QDomElement cc = c.firstChildElement(); !cc.isNull(); cc = cc.nextSiblingElement()) {
      obj.insert(cc.tagName(), attributesOf(cc));
    }
    out.insert(c.tagName(), obj);
  }
  return out;
}
}  // namespace

bool ConsoleBatch::writeReport(const QString& reportFile) const {
  // Build the report from the project XML the GUI would save: the filter settings are
  // private to the filters, but every filter serialises them there.
  QTemporaryFile tmp;
  if (!tmp.open()) {
    return false;
  }
  tmp.close();
  if (!saveProject(tmp.fileName())) {
    return false;
  }
  QFile f(tmp.fileName());
  QDomDocument doc;
  if (!f.open(QIODevice::ReadOnly) || !doc.setContent(&f)) {
    return false;
  }
  const QDomElement root = doc.documentElement();

  // page id -> {image file, sub page}; image id -> file name
  std::map<QString, QString> fileById, dirById, imageFile;
  for (QDomElement d = root.firstChildElement("directories").firstChildElement("directory"); !d.isNull();
       d = d.nextSiblingElement("directory")) {
    dirById[d.attribute("id")] = d.attribute("path");
  }
  for (QDomElement e = root.firstChildElement("files").firstChildElement("file"); !e.isNull();
       e = e.nextSiblingElement("file")) {
    fileById[e.attribute("id")] = dirById[e.attribute("dirId")] + "/" + e.attribute("name");
  }
  for (QDomElement e = root.firstChildElement("images").firstChildElement("image"); !e.isNull();
       e = e.nextSiblingElement("image")) {
    imageFile[e.attribute("id")] = fileById[e.attribute("fileId")];
  }

  struct PageEntry {
    QJsonObject obj;
    QJsonObject stages;
  };
  std::map<QString, PageEntry> pages;  // by page id
  std::vector<QString> order;
  std::map<QString, QString> pageImage;
  for (QDomElement p = root.firstChildElement("pages").firstChildElement("page"); !p.isNull();
       p = p.nextSiblingElement("page")) {
    const QString id = p.attribute("id");
    PageEntry entry;
    entry.obj.insert("id", id);
    entry.obj.insert("image", imageFile[p.attribute("imageId")]);
    entry.obj.insert("subPage", p.attribute("subPage"));
    pages[id] = entry;
    pageImage[id] = p.attribute("imageId");
    order.push_back(id);
  }

  // Output file of each page, matched by (source image, sub page).
  std::map<QString, QString> outputByKey;
  for (const PageInfo& page : m_pages->toPageSequence(PAGE_VIEW)) {
    outputByKey[page.imageId().filePath() + "|" + page.id().subPageAsString()]
        = m_outFileNameGen.filePathFor(page.id());
  }
  for (const QString& id : order) {
    QJsonObject& obj = pages[id].obj;
    const QString key = obj.value("image").toString() + "|" + obj.value("subPage").toString();
    const auto it = outputByKey.find(key);
    if (it != outputByKey.end()) {
      obj.insert("output", it->second);
    }
  }

  const QDomElement filters = root.firstChildElement("filters");
  for (QDomElement fl = filters.firstChildElement(); !fl.isNull(); fl = fl.nextSiblingElement()) {
    const QString stage = fl.tagName();
    for (QDomElement p = fl.firstChildElement(); !p.isNull(); p = p.nextSiblingElement()) {
      if (p.tagName() == "page" && pages.count(p.attribute("id"))) {
        PageEntry& e = pages[p.attribute("id")];
        e.stages.insert(stage, dumpSettings(p));
      } else if (p.tagName() == "image") {
        // fix_orientation and page_split store settings per image, not per page.
        for (const QString& id : order) {
          if (pageImage[id] == p.attribute("id")) {
            pages[id].stages.insert(stage, dumpSettings(p));
          }
        }
      }
    }
  }

  QJsonArray arr;
  for (const QString& id : order) {
    QJsonObject obj = pages[id].obj;
    obj.insert("stages", pages[id].stages);
    arr.append(obj);
  }
  QJsonObject top;
  top.insert("outputDirectory", m_outFileNameGen.outDir());
  top.insert("pages", arr);

  QFile out(reportFile);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  out.write(QJsonDocument(top).toJson(QJsonDocument::Indented));
  return true;
}
