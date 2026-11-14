// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
// Based on ConsoleBatch of Scan Tailor 0.9 (Joseph Artsimovich, Petr Kovář).

#include "BatchProcessor.h"

#include <core/Utils.h>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>
#include <vector>

#include "CompositeCacheDrivenTask.h"
#include "ContentBoxPropagator.h"
#include "Dpi.h"
#include "FileNameDisambiguator.h"
#include "FilterResult.h"
#include "ImageFileInfo.h"
#include "ImageMetadata.h"
#include "ImageMetadataLoader.h"
#include "PageInfo.h"
#include "PageOrientationPropagator.h"
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

namespace {
// The filters ask for the page selection only from their option widgets ("Apply to ...").
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

// QRunnable::create() needs Qt 5.15.
class FunctionRunnable : public QRunnable {
 public:
  explicit FunctionRunnable(std::function<void()> function) : m_function(std::move(function)) {}

  void run() override { m_function(); }

 private:
  std::function<void()> m_function;
};
}  // namespace

BatchProcessor::BatchProcessor(const QStringList& imageFiles,
                               const QString& outputDir,
                               const Qt::LayoutDirection layoutDirection,
                               const int forceDpi) {
  std::vector<ImageFileInfo> files;
  for (const QString& path : imageFiles) {
    std::vector<ImageMetadata> perPage;
    const ImageMetadataLoader::Status status
        = ImageMetadataLoader::load(path, [&](const ImageMetadata& metadata) { perPage.push_back(metadata); });
    if (status != ImageMetadataLoader::LOADED) {
      throw std::runtime_error(("cannot read image: " + path).toStdString());
    }
    for (ImageMetadata& metadata : perPage) {
      if (forceDpi > 0) {
        metadata.setDpi(Dpi(forceDpi, forceDpi));
      } else if (!metadata.isDpiOK()) {
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

BatchProcessor::BatchProcessor(const QString& projectFile, const QString& outputDir) {
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

BatchProcessor::~BatchProcessor() = default;

void BatchProcessor::init(const QString& outputDir, const std::shared_ptr<FileNameDisambiguator>& disambiguator) {
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
  m_taskFactory = std::make_unique<CompositeTaskFactory>(m_stages, m_pages, m_thumbnailCache, m_outFileNameGen);
}

int BatchProcessor::numStages() const {
  return m_stages->count();
}

void BatchProcessor::enterStage(const int stage, const int prevStage) {
  // As MainWindow::filterSelectionChanged when the user selects the stage.
  const StageSequence::Propagation propagation = m_stages->propagationOnStageChange(prevStage, stage);
  if (propagation.contentBoxes) {
    ContentBoxPropagator(m_stages->pageLayoutFilter(),
                         m_taskFactory->createCompositeCacheDrivenTask(m_stages->selectContentFilterIdx()))
        .propagate(*m_pages);
  }
  if (propagation.pageOrientations) {
    PageOrientationPropagator(m_stages->pageSplitFilter(),
                              m_taskFactory->createCompositeCacheDrivenTask(m_stages->fixOrientationFilterIdx()))
        .propagate(*m_pages);
  }
}

int BatchProcessor::process(const Options& options) {
  const int last = numStages() - 1;
  if ((options.startFilter < 0) || (options.startFilter > last) || (options.endFilter < options.startFilter)
      || (options.endFilter > last)) {
    throw std::runtime_error("stage range out of bounds");
  }

  QThreadPool pool;
  pool.setMaxThreadCount(std::max(1, options.threads));
  std::atomic<int> failed{0};
  QMutex callbackMutex;

  for (int stage = options.startFilter; stage <= options.endFilter; ++stage) {
    // A run that starts at a stage enters it from nowhere (-1), so whatever lies above is propagated.
    enterStage(stage, (stage == options.startFilter) ? -1 : stage - 1);
    // Re-read the page sequence every stage: "Split Pages" may change the number of pages.
    const PageSequence pages = m_pages->toPageSequence(PAGE_VIEW);
    // As MainWindow::startBatchProcessing: pages without settings get the default
    // parameters of every stage before their tasks are created.
    for (const PageInfo& page : pages) {
      m_stages->loadDefaultSettings(page);
    }
    if (options.stageStarted) {
      options.stageStarted(stage, static_cast<int>(pages.numPages()));
    }
    for (const PageInfo& page : pages) {
      const BackgroundTaskPtr task = m_taskFactory->createCompositeTask(page, stage, /*batch=*/true, /*debug=*/false);
      pool.start(new FunctionRunnable([task, page, &failed, &callbackMutex, &options]() {
        QString error;
        try {
          const FilterResultPtr result = (*task)();
          if (!result || !result->filter()) {
            // LoadFileTask returns a result without a filter when the image cannot be loaded.
            error = QStringLiteral("cannot process");
          }
        } catch (const std::exception& e) {
          error = QString::fromStdString(e.what());
        }
        if (!error.isEmpty()) {
          ++failed;
        }
        QMutexLocker locker(&callbackMutex);
        if (error.isEmpty()) {
          if (options.pageProcessed) {
            options.pageProcessed(page);
          }
        } else if (options.pageFailed) {
          options.pageFailed(page, error);
        }
      }));
    }
    pool.waitForDone();
  }
  return failed.load();
}

bool BatchProcessor::saveProject(const QString& projectFile) const {
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
