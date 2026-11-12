// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
// Based on ConsoleBatch of Scan Tailor 0.9 (Joseph Artsimovich, Petr Kovář).

#ifndef SCANTAILOR_CORE_BATCHPROCESSOR_H_
#define SCANTAILOR_CORE_BATCHPROCESSOR_H_

#include <QString>
#include <QStringList>
#include <functional>
#include <iosfwd>
#include <memory>

#include "CompositeTaskFactory.h"
#include "NonCopyable.h"
#include "OutputFileNameGenerator.h"

class FileNameDisambiguator;
class PageInfo;
class ProjectPages;
class ProjectReader;
class StageSequence;
class ThumbnailPixmapCache;

/**
 * \brief Headless batch processing of a project over the stage sequence.
 *
 * Runs the stages one after another over all pages, like a GUI user who batch-processes
 * each stage in turn (the "Margins" stage needs the content boxes of all pages before
 * the "Output" stage). Before each stage, pages without settings get the default
 * parameters, and content boxes / page orientations are propagated on the same stage
 * changes as in the GUI. Pages that already have settings (e.g. set by hand in the GUI)
 * keep them.
 *
 * Needs a QApplication (the filters own their option widgets); no window is shown.
 */
class BatchProcessor {
  DECLARE_NON_COPYABLE(BatchProcessor)

 public:
  struct Options {
    /** First and last stage to run, as indices into the stage sequence. */
    int startFilter = 0;
    int endFilter = 5;
    /** Pages processed in parallel within a stage. */
    int threads = 1;
    // Layout toggles (select_content::layout::IncludeFlag) set on every page before stage 4;
    // -1 = each page keeps its own.
    int layoutInclude = -1;

    // Progress callbacks, optional. They are never called concurrently,
    // but may be called from worker threads.
    std::function<void(int stage, int numPages)> stageStarted;
    std::function<void(const PageInfo& page)> pageProcessed;
    std::function<void(const PageInfo& page, const QString& error)> pageFailed;
  };

  /**
   * New project from image files; pages are split automatically (AUTO_PAGES).
   * forceDpi > 0 replaces the resolution stored in the files (needed when it is missing
   * or wrong); otherwise a file without a usable resolution is an error.
   *
   * \throws std::runtime_error if an image cannot be read.
   */
  BatchProcessor(const QStringList& imageFiles,
                 const QString& outputDir,
                 Qt::LayoutDirection layoutDirection,
                 int forceDpi);

  /**
   * Existing project. An empty outputDir keeps the project's own output directory.
   *
   * \throws std::runtime_error if the project cannot be loaded.
   */
  BatchProcessor(const QString& projectFile, const QString& outputDir);

  ~BatchProcessor();

  /**
   * Runs the stages options.startFilter to options.endFilter.
   * Returns the number of page tasks that failed (0 = all fine).
   *
   * \throws std::runtime_error if the stage range is out of bounds.
   */
  int process(const Options& options);

  bool saveProject(const QString& projectFile) const;

  int numStages() const;

  const std::shared_ptr<ProjectPages>& pages() const { return m_pages; }

  const std::shared_ptr<StageSequence>& stages() const { return m_stages; }

  const OutputFileNameGenerator& outputFileNameGenerator() const { return m_outFileNameGen; }

  /** Deskew fallback on low confidence (issue #15), stored in the project; default on. */
  void setDeskewFallback(bool enabled);

  /**
   * One warning line per page whose automatic deskew or oblique is marked uncertain (issue #15).
   * Returns the number of such pages.
   */
  int printUncertainDeskew(std::ostream& out) const;

 private:
  void init(const QString& outputDir, const std::shared_ptr<FileNameDisambiguator>& disambiguator);

  void enterStage(int stage, int prevStage);

  std::shared_ptr<ProjectPages> m_pages;
  std::shared_ptr<StageSequence> m_stages;
  std::shared_ptr<ThumbnailPixmapCache> m_thumbnailCache;
  OutputFileNameGenerator m_outFileNameGen;
  std::unique_ptr<CompositeTaskFactory> m_taskFactory;
  std::unique_ptr<ProjectReader> m_reader;
};


#endif  // SCANTAILOR_CORE_BATCHPROCESSOR_H_
