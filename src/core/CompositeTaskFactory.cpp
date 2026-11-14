// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "CompositeTaskFactory.h"

#include <cassert>

#include "CompositeCacheDrivenTask.h"
#include "LoadFileTask.h"
#include "PageInfo.h"
#include "ProjectPages.h"
#include "StageSequence.h"
#include "ThumbnailPixmapCache.h"
#include "filters/deskew/CacheDrivenTask.h"
#include "filters/deskew/Task.h"
#include "filters/fix_orientation/CacheDrivenTask.h"
#include "filters/fix_orientation/Task.h"
#include "filters/output/CacheDrivenTask.h"
#include "filters/output/Task.h"
#include "filters/page_layout/CacheDrivenTask.h"
#include "filters/page_layout/Task.h"
#include "filters/page_split/CacheDrivenTask.h"
#include "filters/page_split/Task.h"
#include "filters/select_content/CacheDrivenTask.h"
#include "filters/select_content/Task.h"

CompositeTaskFactory::CompositeTaskFactory(std::shared_ptr<StageSequence> stages,
                                           std::shared_ptr<ProjectPages> pages,
                                           std::shared_ptr<ThumbnailPixmapCache> thumbnailCache,
                                           const OutputFileNameGenerator& outFileNameGen)
    : m_stages(std::move(stages)),
      m_pages(std::move(pages)),
      m_thumbnailCache(std::move(thumbnailCache)),
      m_outFileNameGen(outFileNameGen) {}

BackgroundTaskPtr CompositeTaskFactory::createCompositeTask(const PageInfo& page,
                                                            const int lastFilterIdx,
                                                            const bool batch,
                                                            bool debug) const {
  std::shared_ptr<fix_orientation::Task> fixOrientationTask;
  std::shared_ptr<page_split::Task> pageSplitTask;
  std::shared_ptr<deskew::Task> deskewTask;
  std::shared_ptr<select_content::Task> selectContentTask;
  std::shared_ptr<page_layout::Task> pageLayoutTask;
  std::shared_ptr<output::Task> outputTask;

  if (batch) {
    debug = false;
  }

  if (lastFilterIdx >= m_stages->outputFilterIdx()) {
    outputTask = m_stages->outputFilter()->createTask(page.id(), m_thumbnailCache, m_outFileNameGen, batch, debug);
    debug = false;
  }
  if (lastFilterIdx >= m_stages->pageLayoutFilterIdx()) {
    pageLayoutTask = m_stages->pageLayoutFilter()->createTask(page.id(), outputTask, batch, debug);
    debug = false;
  }
  if (lastFilterIdx >= m_stages->selectContentFilterIdx()) {
    selectContentTask = m_stages->selectContentFilter()->createTask(page.id(), pageLayoutTask, batch, debug);
    debug = false;
  }
  if (lastFilterIdx >= m_stages->deskewFilterIdx()) {
    deskewTask = m_stages->deskewFilter()->createTask(page.id(), selectContentTask, batch, debug);
    debug = false;
  }
  if (lastFilterIdx >= m_stages->pageSplitFilterIdx()) {
    pageSplitTask = m_stages->pageSplitFilter()->createTask(page, deskewTask, batch, debug);
    debug = false;
  }
  if (lastFilterIdx >= m_stages->fixOrientationFilterIdx()) {
    fixOrientationTask = m_stages->fixOrientationFilter()->createTask(page.id(), pageSplitTask, batch);
    debug = false;
  }
  assert(fixOrientationTask);
  return std::make_shared<LoadFileTask>(batch ? BackgroundTask::BATCH : BackgroundTask::INTERACTIVE, page,
                                        m_thumbnailCache, m_pages, fixOrientationTask);
}

std::shared_ptr<CompositeCacheDrivenTask> CompositeTaskFactory::createCompositeCacheDrivenTask(
    const int lastFilterIdx) const {
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
  if (lastFilterIdx >= m_stages->fixOrientationFilterIdx()) {
    fixOrientationTask = m_stages->fixOrientationFilter()->createCacheDrivenTask(pageSplitTask);
  }

  assert(fixOrientationTask);
  return fixOrientationTask;
}
