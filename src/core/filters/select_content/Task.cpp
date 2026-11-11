// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Task.h"

#include <UnitsProvider.h>

#include <iostream>
#include <utility>

#include "ContentBoxFinder.h"
#include "DebugImagesImpl.h"
#include "Dpm.h"
#include "Filter.h"
#include "FilterData.h"
#include "FilterUiInterface.h"
#include "ImageView.h"
#include "LayoutDetector.h"
#include "OptionsWidget.h"
#include "PageFinder.h"
#include "TaskStatus.h"
#include "filters/page_layout/Task.h"

using namespace imageproc;

namespace select_content {
class Task::UiUpdater : public FilterResult {
 public:
  UiUpdater(std::shared_ptr<Filter> filter,
            const PageId& pageId,
            std::unique_ptr<DebugImages> dbg,
            const QImage& image,
            const ImageTransformation& xform,
            const ContentMask& contentMask,
            const OptionsWidget::UiData& uiData,
            std::shared_ptr<const LayoutResult> layout,
            bool batch);

  void updateUI(FilterUiInterface* ui) override;

  std::shared_ptr<AbstractFilter> filter() override { return m_filter; }

 private:
  std::shared_ptr<Filter> m_filter;
  PageId m_pageId;
  std::unique_ptr<DebugImages> m_dbg;
  QImage m_image;
  QImage m_downscaledImage;
  ContentMask m_contentMask;
  ImageTransformation m_xform;
  OptionsWidget::UiData m_uiData;
  std::shared_ptr<const LayoutResult> m_layout;
  bool m_batchProcessing;
};


Task::Task(std::shared_ptr<Filter> filter,
           std::shared_ptr<page_layout::Task> nextTask,
           std::shared_ptr<Settings> settings,
           const PageId& pageId,
           const bool batch,
           const bool debug)
    : m_filter(std::move(filter)),
      m_nextTask(std::move(nextTask)),
      m_settings(std::move(settings)),
      m_pageId(pageId),
      m_batchProcessing(batch) {
  if (debug) {
    m_dbg = std::make_unique<DebugImagesImpl>();
  }
}

Task::~Task() = default;

FilterResultPtr Task::process(const TaskStatus& status, const FilterData& data) {
  status.throwIfCancelled();

  std::unique_ptr<Params> params(m_settings->getPageParams(m_pageId));
  const Dependencies deps = (params) ? Dependencies(data.xform().resultingPreCropArea(), params->contentDetectionMode(),
                                                    params->pageDetectionMode(), params->isFineTuningEnabled())
                                     : Dependencies(data.xform().resultingPreCropArea());

  Params newParams(deps);
  if (params) {
    newParams = *params;
    newParams.setDependencies(deps);
  }

  const PhysSizeCalc physSizeCalc(data.xform());

  bool needUpdateContentBox = false;
  bool needUpdatePageBox = false;

  if (!params || !deps.compatibleWith(params->dependencies(), &needUpdateContentBox, &needUpdatePageBox)) {
    QRectF pageRect(newParams.pageRect());
    QRectF contentRect(newParams.contentRect());

    if (needUpdatePageBox) {
      const QRectF oldPageRect(pageRect);

      if (newParams.pageDetectionMode() == MODE_AUTO) {
        pageRect
            = PageFinder::findPageBox(status, data, newParams.isFineTuningEnabled(), m_settings->pageDetectionBox(),
                                      m_settings->pageDetectionTolerance(), m_dbg.get());
      } else if (newParams.pageDetectionMode() == MODE_DISABLED) {
        pageRect = data.xform().resultingRect();
      }

      if (!data.xform().resultingRect().intersected(pageRect).isValid()) {
        pageRect = data.xform().resultingRect();
      }

      // When offcut/page outline changes, keep manual content aligned with page motion (issue #90).
      if (newParams.contentDetectionMode() == MODE_MANUAL && contentRect.isValid() && oldPageRect.isValid()
          && pageRect.isValid()) {
        contentRect.translate(pageRect.center() - oldPageRect.center());
        contentRect &= pageRect;
        if (!contentRect.isValid()) {
          needUpdateContentBox = true;
        }
      } else if (contentRect.isValid() && (contentRect.intersected(pageRect) != contentRect)) {
        if (newParams.contentDetectionMode() == MODE_MANUAL) {
          contentRect = contentRect.intersected(pageRect);
        } else {
          needUpdateContentBox = true;
        }
      }

      newParams.setPageRect(pageRect);
    }

    if (needUpdateContentBox) {
      if (newParams.contentDetectionMode() == MODE_AUTO) {
        contentRect = ContentBoxFinder::findContentBox(status, data, pageRect, m_dbg.get());
      } else if (newParams.contentDetectionMode() == MODE_DISABLED) {
        contentRect = pageRect;
      } else if (newParams.contentDetectionMode() == MODE_MANUAL) {
        if (contentRect.isValid() && pageRect.isValid()) {
          contentRect &= pageRect;
        } else {
          contentRect = QRectF();
        }
      }

      if (contentRect.isValid()) {
        contentRect &= pageRect;
      }

      newParams.setContentRect(contentRect);
      newParams.setContentSizeMM(physSizeCalc.sizeMM(contentRect));
    } else if (contentRect.isValid()) {
      newParams.setContentRect(contentRect);
      newParams.setContentSizeMM(physSizeCalc.sizeMM(contentRect));
    }
  }

  status.throwIfCancelled();

  const AutoManualMode mode = newParams.contentDetectionMode();
  // The automatic content box: just found, or kept from before (Params::autoContentRect while
  // the layout toggles change the content box; else the content box itself).
  QRectF autoBox;
  if (mode == MODE_AUTO) {
    const bool justFound = needUpdateContentBox;
    autoBox = (!justFound && newParams.autoContentRect()) ? *newParams.autoContentRect() : newParams.contentRect();
  }

  // Layout detector (only with a model configured): page numbers, tabs, headers, marginalia.
  std::shared_ptr<const LayoutResult> layout;
  if (const std::shared_ptr<LayoutDetector> detector = LayoutDetector::instance()) {
    layout = m_settings->layoutResult(m_pageId);
    if (!layout || !layout->matches(data.xform(), newParams.pageRect(), detector->modelSha256())
        || (mode == MODE_AUTO && layout->autoBox != autoBox)) {
      // The side-strip tab rule needs the automatic content box, whatever the page's mode.
      const QRectF tabRuleBox = (mode == MODE_AUTO)
                                    ? autoBox
                                    : ContentBoxFinder::findContentBox(status, data, newParams.pageRect(), nullptr);
      try {
        layout = detector->analyze(data.origImage(), data.xform(), newParams.pageRect(), tabRuleBox);
        m_settings->setLayoutResult(m_pageId, layout);
      } catch (const std::exception& e) {
        std::cerr << "layout detector: " << m_pageId.imageId().filePath().toStdString() << ": " << e.what()
                  << std::endl;
        layout.reset();
      }
    }
    status.throwIfCancelled();
  }

  // Layout toggles: only the automatic content box is changed (a box set by hand never is).
  if (mode == MODE_AUTO) {
    const unsigned include = newParams.layoutInclude();
    if (include == 0) {
      if (newParams.autoContentRect()) {  // toggles switched off: back to the automatic box
        newParams.setContentRect(autoBox);
        newParams.setContentSizeMM(physSizeCalc.sizeMM(autoBox));
        newParams.setAutoContentRect(std::nullopt);
      }
    } else {
      newParams.setAutoContentRect(autoBox);
      // Without a model (or if it failed) the content box stays as it is.
      if (layout) {
        const layout::FinalBox fb = layout->finalBox(autoBox, include, pageSideOf(m_pageId));
        const QRectF contentRect = fb.box ? layoutBoxToRect(*fb.box) : QRectF();
        newParams.setContentRect(contentRect);
        newParams.setContentSizeMM(physSizeCalc.sizeMM(contentRect));
      }
    }
  }

  OptionsWidget::UiData uiData;
  uiData.setSizeCalc(physSizeCalc);
  uiData.setContentRect(newParams.contentRect());
  uiData.setPageRect(newParams.pageRect());
  uiData.setDependencies(deps);
  uiData.setContentDetectionMode(newParams.contentDetectionMode());
  uiData.setPageDetectionMode(newParams.pageDetectionMode());
  uiData.setFineTuneCornersEnabled(newParams.isFineTuningEnabled());
  uiData.setLayoutInclude(newParams.layoutInclude());
  uiData.setAutoContentRect(newParams.autoContentRect());

  m_settings->setPageParams(m_pageId, newParams);

  status.throwIfCancelled();

  if (m_nextTask) {
    return m_nextTask->process(status, FilterData(data, data.xform()), uiData.pageRect(), uiData.contentRect());
  } else {
    return std::make_shared<UiUpdater>(m_filter, m_pageId, std::move(m_dbg), data.origImage(), data.xform(),
                                       ContentMask(data.grayImageBlackOnWhite(), data.xform(), status), uiData,
                                       layout, m_batchProcessing);
  }
}  // Task::process

/*============================ Task::UiUpdater ==========================*/

Task::UiUpdater::UiUpdater(std::shared_ptr<Filter> filter,
                           const PageId& pageId,
                           std::unique_ptr<DebugImages> dbg,
                           const QImage& image,
                           const ImageTransformation& xform,
                           const ContentMask& contentMask,
                           const OptionsWidget::UiData& uiData,
                           std::shared_ptr<const LayoutResult> layout,
                           const bool batch)
    : m_filter(std::move(filter)),
      m_pageId(pageId),
      m_dbg(std::move(dbg)),
      m_image(image),
      m_downscaledImage(ImageView::createDownscaledImage(image)),
      m_contentMask(contentMask),
      m_xform(xform),
      m_uiData(uiData),
      m_layout(std::move(layout)),
      m_batchProcessing(batch) {}

void Task::UiUpdater::updateUI(FilterUiInterface* ui) {
  // This function is executed from the GUI thread.
  ui->invalidateThumbnail(m_pageId);

  if (m_batchProcessing) {
    return;
  }

  OptionsWidget* const optWidget = m_filter->optionsWidget();
  optWidget->postUpdateUI(m_uiData);
  ui->setOptionsWidget(optWidget, ui->KEEP_OWNERSHIP);

  auto* view = new ImageView(m_image, m_downscaledImage, m_contentMask, m_xform, m_uiData.contentRect(),
                             m_uiData.pageRect(), m_uiData.pageDetectionMode() != MODE_DISABLED);
  if (m_layout) {
    view->setLayoutElements(m_layout->elements);
  }
  ui->setImageWidget(view, ui->TRANSFER_OWNERSHIP, m_dbg.get());

  QObject::connect(view, SIGNAL(manualContentRectSet(const QRectF&)), optWidget,
                   SLOT(manualContentRectSet(const QRectF&)));
  QObject::connect(view, SIGNAL(manualPageRectSet(const QRectF&)), optWidget, SLOT(manualPageRectSet(const QRectF&)));
  QObject::connect(view, SIGNAL(pageRectSizeChanged(const QSizeF&)), optWidget,
                   SLOT(updatePageRectSize(const QSizeF&)));
  QObject::connect(optWidget, SIGNAL(pageRectChangedLocally(const QRectF&)), view,
                   SLOT(pageRectSetExternally(const QRectF&)));
  QObject::connect(optWidget, SIGNAL(pageRectStateChanged(bool)), view, SLOT(setPageRectEnabled(bool)));
}
}  // namespace select_content