// Copyright (C) 2026  scantailor-cli contributors
// Based on ConsoleBatch of Scan Tailor 0.9.x (Joseph Artsimovich, Petr Kovar), GPLv3.
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_CONSOLEBATCH_H_
#define SCANTAILOR_APP_CONSOLEBATCH_H_

#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "BackgroundTask.h"
#include "OutputFileNameGenerator.h"
#include "PageInfo.h"

class ProjectPages;
class StageSequence;
class ThumbnailPixmapCache;
class ProjectReader;

/**
 * Headless batch processing of a ScanTailor project.
 *
 * Runs the six stages one after another over all pages (stage by stage, like the GUI user
 * who batch-processes each stage in turn; the page-layout stage needs the content boxes of
 * all pages before the output stage). Pages that already have settings — e.g. corrected by
 * hand in the GUI — keep them: the filters only fall back to the default parameters for
 * pages without settings.
 */
class ConsoleBatch {
 public:
  struct Options {
    int startFilter = 0;  // 0-based stage index
    int endFilter = 5;
    int threads = 1;
    bool verbose = false;
  };

  /**
   * New project from image files; pages are split automatically (AUTO_PAGES).
   * forceDpi > 0 replaces the resolution stored in the files (needed when it is missing
   * or wrong, e.g. 72 dpi); otherwise a file without a usable resolution is an error.
   */
  ConsoleBatch(const QStringList& imageFiles,
               const QString& outputDir,
               Qt::LayoutDirection layoutDirection,
               int forceDpi);

  /** Existing project. An empty outputDir keeps the project's own output directory. */
  ConsoleBatch(const QString& projectFile, const QString& outputDir);

  ~ConsoleBatch();

  /** Runs the stages. Returns the number of pages that failed (0 = all fine). */
  int process(const Options& options);

  bool saveProject(const QString& projectFile) const;

  /** Per-page JSON report built from the project as it would be saved now. */
  bool writeReport(const QString& reportFile) const;

  int numFilters() const;

 private:
  void init(const QString& outputDir, const std::shared_ptr<class FileNameDisambiguator>& disambiguator);

  BackgroundTaskPtr createCompositeTask(const PageInfo& page, int lastFilterIdx);

  std::shared_ptr<class CompositeCacheDrivenTask> createCompositeCacheDrivenTask(int lastFilterIdx);

  void enterStage(int stage, bool firstStage);

  std::shared_ptr<ProjectPages> m_pages;
  std::shared_ptr<StageSequence> m_stages;
  std::shared_ptr<ThumbnailPixmapCache> m_thumbnailCache;
  OutputFileNameGenerator m_outFileNameGen;
  std::unique_ptr<ProjectReader> m_reader;
};

#endif  // SCANTAILOR_APP_CONSOLEBATCH_H_
