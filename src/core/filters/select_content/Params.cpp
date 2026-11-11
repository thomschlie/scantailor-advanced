// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "Params.h"

#include "LayoutRules.h"

#include "XmlMarshaller.h"
#include "XmlUnmarshaller.h"

namespace {
// Layout detector toggles: one attribute each, written only when on (old projects and projects
// without toggles stay as they were; a missing attribute is off).
struct IncludeAttr {
  unsigned flag;
  const char* name;
};

const IncludeAttr kIncludeAttrs[] = {
    {select_content::layout::kIncludePageNumbers, "includePageNumbers"},
    {select_content::layout::kIncludeTabs, "includeTabs"},
    {select_content::layout::kIncludeHeaders, "includeHeaders"},
    {select_content::layout::kIncludeMarginalia, "includeMarginalia"},
    {select_content::layout::kCleanPaperEdge, "cleanPaperEdge"},
};
}  // namespace

namespace select_content {
Params::Params(const Dependencies& deps)
    : m_deps(deps), m_contentDetectionMode(MODE_AUTO), m_pageDetectionMode(MODE_DISABLED), m_fineTuneCorners(false) {}

Params::Params(const QRectF& contentRect,
               const QSizeF& contentSizeMm,
               const QRectF& pageRect,
               const Dependencies& deps,
               const AutoManualMode contentDetectionMode,
               const AutoManualMode pageDetectionMode,
               const bool fineTuneCorners)
    : m_contentRect(contentRect),
      m_pageRect(pageRect),
      m_contentSizeMM(contentSizeMm),
      m_deps(deps),
      m_contentDetectionMode(contentDetectionMode),
      m_pageDetectionMode(pageDetectionMode),
      m_fineTuneCorners(fineTuneCorners) {}

Params::Params(const QDomElement& filterEl)
    : m_contentRect(XmlUnmarshaller::rectF(filterEl.namedItem("content-rect").toElement())),
      m_pageRect(XmlUnmarshaller::rectF(filterEl.namedItem("page-rect").toElement())),
      m_contentSizeMM(XmlUnmarshaller::sizeF(filterEl.namedItem("content-size-mm").toElement())),
      m_deps(filterEl.namedItem("dependencies").toElement()),
      m_contentDetectionMode(stringToAutoManualMode(filterEl.attribute("contentDetectionMode"))),
      m_pageDetectionMode(stringToAutoManualMode(filterEl.attribute("pageDetectionMode"))),
      m_fineTuneCorners(filterEl.attribute("fineTuneCorners") == "1") {
  for (const IncludeAttr& a : kIncludeAttrs) {
    if (filterEl.attribute(a.name) == "1") {
      m_layoutInclude |= a.flag;
    }
  }
  const QDomElement autoEl = filterEl.namedItem("auto-content-rect").toElement();
  if (!autoEl.isNull()) {
    m_autoContentRect = XmlUnmarshaller::rectF(autoEl);
  }
}

Params::~Params() = default;

QDomElement Params::toXml(QDomDocument& doc, const QString& name) const {
  XmlMarshaller marshaller(doc);

  QDomElement el(doc.createElement(name));
  el.setAttribute("contentDetectionMode", autoManualModeToString(m_contentDetectionMode));
  el.setAttribute("pageDetectionMode", autoManualModeToString(m_pageDetectionMode));
  el.setAttribute("fineTuneCorners", m_fineTuneCorners ? "1" : "0");
  for (const IncludeAttr& a : kIncludeAttrs) {
    if (m_layoutInclude & a.flag) {
      el.setAttribute(a.name, "1");
    }
  }
  el.appendChild(marshaller.rectF(m_contentRect, "content-rect"));
  el.appendChild(marshaller.rectF(m_pageRect, "page-rect"));
  el.appendChild(marshaller.sizeF(m_contentSizeMM, "content-size-mm"));
  el.appendChild(m_deps.toXml(doc, "dependencies"));
  if (m_autoContentRect) {
    el.appendChild(marshaller.rectF(*m_autoContentRect, "auto-content-rect"));
  }
  return el;
}
}  // namespace select_content
