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
#include <cmath>
#include <ostream>
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
#include "filters/deskew/Filter.h"
#include "filters/deskew/Settings.h"
#include "filters/page_layout/Filter.h"
#include "filters/page_layout/Params.h"
#include "filters/page_layout/Settings.h"
#include "filters/page_layout/Utils.h"
#include "filters/select_content/Filter.h"

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

bool sameMarginsMm(const Margins& a, const Margins& b) {
  // Stored as text in the project file; a thousandth of a millimetre is far below a pixel.
  const double eps = 1e-3;
  return std::abs(a.left() - b.left()) < eps && std::abs(a.top() - b.top()) < eps
         && std::abs(a.right() - b.right()) < eps && std::abs(a.bottom() - b.bottom()) < eps;
}

// --reset-layout: whether a page's stored page layout looks automatic (BatchProcessor::ResetLayout).
bool isAutomaticPageLayout(const page_layout::Params& params,
                           const page_layout::Params& profileDefaults,
                           const page_layout::Params& fromOptions) {
  using page_layout::Alignment;
  const bool asProfile = sameMarginsMm(params.hardMarginsMM(), profileDefaults.hardMarginsMM())
                         && params.alignment() == profileDefaults.alignment()
                         && params.isAutoMarginsEnabled() == profileDefaults.isAutoMarginsEnabled();
  const Alignment& alignment = params.alignment();
  const bool outerBottom = !alignment.isNull() && alignment.vertical() == Alignment::BOTTOM
                           && (alignment.horizontal() == Alignment::LEFT || alignment.horizontal() == Alignment::RIGHT);
  const bool asOptions = outerBottom && sameMarginsMm(params.hardMarginsMM(), fromOptions.hardMarginsMM())
                         && params.isAutoMarginsEnabled() == fromOptions.isAutoMarginsEnabled();
  return asProfile || asOptions;
}

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

  const bool pageLayoutOptions
      = options.pageLayoutMarginsMm || options.alignOuterBottom || (options.resetLayout != ResetLayout::OFF);
  const std::shared_ptr<page_layout::Settings>& pageLayoutSettings = m_stages->pageLayoutFilter()->settings();
  // Pages that got the page layout defaults in this run and still wait for the page layout options.
  std::set<PageId> pageLayoutPending;
  bool pageLayoutApplied = false;
  // Checked before any stage runs: an unknown image is an error, not a half-processed project.
  const std::vector<std::pair<QString, PageSide>> pageSideAnchors = resolvePageSideAnchors(options);

  for (int stage = options.startFilter; stage <= options.endFilter; ++stage) {
    // Pages without page layout settings, noted before the content box propagation of
    // enterStage() gives them upstream's built-in ones (page_layout::Settings::setContentSizeMM).
    if (pageLayoutOptions) {
      for (const PageInfo& page : m_pages->toPageSequence(PAGE_VIEW)) {
        if (pageLayoutSettings->isParamsNull(page.id())) {
          pageLayoutPending.insert(page.id());
        }
      }
    }
    // A run that starts at a stage enters it from nowhere (-1), so whatever lies above is propagated.
    enterStage(stage, (stage == options.startFilter) ? -1 : stage - 1);
    // Re-read the page sequence every stage: "Split Pages" may change the number of pages.
    const PageSequence pages = m_pages->toPageSequence(PAGE_VIEW);
    // As MainWindow::startBatchProcessing: pages without settings get the default
    // parameters of every stage before their tasks are created.
    for (const PageInfo& page : pages) {
      if (pageLayoutOptions && pageLayoutSettings->isParamsNull(page.id())) {
        pageLayoutPending.insert(page.id());
      }
      m_stages->loadDefaultSettings(page);
    }
    // --margins-mm / --align / --reset-layout: once the page sequence is final (page split
    // done), before "Margins"; only in the run's first stage >= 5.
    if (pageLayoutOptions && !pageLayoutApplied && stage >= m_stages->pageLayoutFilterIdx()) {
      applyPageLayoutOptions(options, pageLayoutPending, pageSideAnchors);
      pageLayoutPending.clear();
      pageLayoutApplied = true;
    }
    // --include: the layout toggles of every page, set before "Select content" runs.
    if (stage == m_stages->selectContentFilterIdx() && options.layoutInclude >= 0) {
      for (const PageInfo& page : pages) {
        m_stages->selectContentFilter()->setLayoutInclude(page.id(), static_cast<unsigned>(options.layoutInclude));
      }
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

std::vector<std::pair<QString, BatchProcessor::PageSide>> BatchProcessor::resolvePageSideAnchors(
    const Options& options) const {
  std::vector<QString> files;
  for (const PageInfo& page : m_pages->toPageSequence(IMAGE_VIEW)) {
    const QString& file = page.imageId().filePath();
    if (std::find(files.begin(), files.end(), file) == files.end()) {
      files.push_back(file);
    }
  }
  std::vector<std::pair<QString, PageSide>> anchors;
  for (const PageSideAnchor& anchor : options.pageSideAnchors) {
    const bool byPath = anchor.image.contains('/');
    const QString wanted = byPath ? QFileInfo(anchor.image).absoluteFilePath() : anchor.image;
    std::vector<QString> matches;
    for (const QString& file : files) {
      const QFileInfo fi(file);
      if ((byPath ? fi.absoluteFilePath() : fi.fileName()) == wanted) {
        matches.push_back(file);
      }
    }
    if (matches.empty()) {
      throw std::runtime_error(("--page-side: no image file " + anchor.image + " in the project").toStdString());
    }
    if (matches.size() > 1) {
      throw std::runtime_error(
          ("--page-side: " + anchor.image + " names several image files of the project; give its path").toStdString());
    }
    for (const auto& known : anchors) {
      if (known.first == matches.front()) {
        throw std::runtime_error(("--page-side: " + anchor.image + " given twice").toStdString());
      }
    }
    anchors.emplace_back(matches.front(), anchor.side);
  }
  return anchors;
}

void BatchProcessor::applyPageLayoutOptions(const Options& options,
                                            const std::set<PageId>& pageIds,
                                            const std::vector<std::pair<QString, PageSide>>& anchors) const {
  const std::shared_ptr<page_layout::Settings>& settings = m_stages->pageLayoutFilter()->settings();
  const PageSequence pages = m_pages->toPageSequence(PAGE_VIEW);
  const bool rightToLeft = (m_pages->layoutDirection() == Qt::RightToLeft);
  // Single pages: the first one is a recto (or --first-page), then they alternate; an anchor
  // (--page-side) restarts the alternation at the first page of its image file.
  PageSide runSide = options.firstPageSide.value_or(rightToLeft ? PageSide::LEFT : PageSide::RIGHT);
  size_t runStart = 0;
  std::set<QString> anchorsSeen;
  size_t position = 0;
  for (const PageInfo& page : pages) {
    const QString& file = page.imageId().filePath();
    for (const auto& anchor : anchors) {
      if (anchor.first == file && anchorsSeen.insert(file).second) {
        runSide = anchor.second;
        runStart = position;
      }
    }
    std::optional<PageSide> side;
    if (options.alignOuterBottom) {
      switch (page.id().subPage()) {
        case PageId::LEFT_PAGE:
          side = PageSide::LEFT;
          break;
        case PageId::RIGHT_PAGE:
          side = PageSide::RIGHT;
          break;
        default:
          if ((position - runStart) % 2 == 0) {
            side = runSide;
          } else {
            side = (runSide == PageSide::LEFT) ? PageSide::RIGHT : PageSide::LEFT;
          }
          break;
      }
    }
    ++position;

    const std::unique_ptr<page_layout::Params> params = settings->getPageParams(page.id());
    if (!params) {
      continue;  // vanished in page split
    }
    // What the run gives this page: the profile's defaults, then the options on top.
    const page_layout::Params profileDefaults = page_layout::Utils::buildDefaultParams(page.metadata().dpi());
    Margins margins = profileDefaults.hardMarginsMM();
    bool autoMargins = profileDefaults.isAutoMarginsEnabled();
    if (options.pageLayoutMarginsMm) {
      margins = *options.pageLayoutMarginsMm;
      autoMargins = false;
    }
    page_layout::Alignment alignment = profileDefaults.alignment();
    if (side) {
      alignment = page_layout::Alignment(page_layout::Alignment::BOTTOM, (*side == PageSide::LEFT)
                                                                             ? page_layout::Alignment::LEFT
                                                                             : page_layout::Alignment::RIGHT);
    }
    // Page and content rectangles stay as stored; the page layout stage recomputes them.
    const page_layout::Params fromRun(margins, params->pageRect(), params->contentRect(), params->contentSizeMM(),
                                      alignment, autoMargins);

    PageLayoutOutcome outcome = PageLayoutOutcome::SET;
    if (pageIds.count(page.id()) == 0) {
      // Has settings of its own (or got them in an earlier run).
      switch (options.resetLayout) {
        case ResetLayout::OFF:
          outcome = PageLayoutOutcome::KEPT;
          break;
        case ResetLayout::AUTOMATIC:
          if (!isAutomaticPageLayout(*params, profileDefaults, fromRun)) {
            outcome = PageLayoutOutcome::SPARED;
          }
          break;
        case ResetLayout::ALL:
          break;
      }
    }
    if (outcome == PageLayoutOutcome::SET) {
      settings->setPageParams(page.id(), fromRun);
    }
    if (options.pageLayoutDecided) {
      options.pageLayoutDecided(page, outcome, side);
    }
  }
}

void BatchProcessor::setDeskewFallback(const bool enabled) {
  m_stages->deskewFilter()->settings()->setFallbackEnabled(enabled);
}

int BatchProcessor::printUncertainDeskew(std::ostream& out) const {
  const std::shared_ptr<deskew::Settings> settings = m_stages->deskewFilter()->settings();
  int count = 0;
  for (const PageInfo& page : m_pages->toPageSequence(PAGE_VIEW)) {
    const std::unique_ptr<deskew::Params> params = settings->getPageParams(page.id());
    if (!params || !params->isUncertain()) {
      continue;
    }
    ++count;
    const QString outName = QFileInfo(m_outFileNameGen.filePathFor(page.id())).completeBaseName();
    const QString image = QFileInfo(page.imageId().filePath()).fileName();
    out << "warning: deskew uncertain: " << outName.toStdString() << " (" << image.toStdString();
    if (page.id().subPage() != PageId::SINGLE_PAGE) {
      out << " " << page.id().subPageAsString().toStdString();
    }
    out << "):";
    if (params->isAngleUncertain()) {
      out << " angle " << params->deskewAngle() << " not confirmed (SkewFinder confidence "
          << params->confidence().value_or(0.0) << ", fallback not sure)";
    }
    if (params->isObliqueUncertain()) {
      out << (params->isAngleUncertain() ? ";" : "") << " automatic oblique "
          << params->diagnostics().rejectedOblique.value_or(NAN) << " > " << deskew::MAX_PLAUSIBLE_OBLIQUE
          << ", set to 0";
    }
    out << std::endl;
  }
  return count;
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
