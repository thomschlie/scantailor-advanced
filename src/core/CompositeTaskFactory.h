// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_COMPOSITETASKFACTORY_H_
#define SCANTAILOR_CORE_COMPOSITETASKFACTORY_H_

#include <memory>

#include "BackgroundTask.h"
#include "OutputFileNameGenerator.h"

class CompositeCacheDrivenTask;
class PageInfo;
class ProjectPages;
class StageSequence;
class ThumbnailPixmapCache;

/**
 * \brief Builds the chains of filter tasks that process a page up to a given stage.
 *
 * Shared by the GUI (MainWindow) and headless batch processing, so both process
 * a page the same way.
 */
class CompositeTaskFactory {
 public:
  CompositeTaskFactory(std::shared_ptr<StageSequence> stages,
                       std::shared_ptr<ProjectPages> pages,
                       std::shared_ptr<ThumbnailPixmapCache> thumbnailCache,
                       const OutputFileNameGenerator& outFileNameGen);

  /**
   * A task that loads the page's image and runs every stage up to and including
   * lastFilterIdx. Only the last stage gets the debug flag, and never in batch mode.
   */
  BackgroundTaskPtr createCompositeTask(const PageInfo& page, int lastFilterIdx, bool batch, bool debug) const;

  /** The cache-driven counterpart (thumbnails, propagators), up to and including lastFilterIdx. */
  std::shared_ptr<CompositeCacheDrivenTask> createCompositeCacheDrivenTask(int lastFilterIdx) const;

 private:
  std::shared_ptr<StageSequence> m_stages;
  std::shared_ptr<ProjectPages> m_pages;
  std::shared_ptr<ThumbnailPixmapCache> m_thumbnailCache;
  OutputFileNameGenerator m_outFileNameGen;
};


#endif  // SCANTAILOR_CORE_COMPOSITETASKFACTORY_H_
