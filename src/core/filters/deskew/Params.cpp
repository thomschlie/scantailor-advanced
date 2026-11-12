// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
// Split Params: Zvezdochiot; dev branch implements as requested (see Params.h).

#include "Params.h"

#include <foundation/Utils.h>

#include <QDomDocument>
#include <QStringList>

using namespace foundation;

namespace deskew {
namespace {

AutoManualMode obliqueModeFromXml(const QDomElement& deskewEl) {
  if (deskewEl.hasAttribute("oblique-mode")) {
    return deskewEl.attribute("oblique-mode") == "manual" ? MODE_MANUAL : MODE_AUTO;
  }
  if (deskewEl.hasAttribute("autoOblique")) {
    return deskewEl.attribute("autoOblique", "1") != "0" ? MODE_AUTO : MODE_MANUAL;
  }
  return deskewEl.attribute("mode") == "manual" ? MODE_MANUAL : MODE_AUTO;
}

AutoDiagnostics diagnosticsFromXml(const QDomElement& el) {
  AutoDiagnostics d;
  bool ok = false;
  const double conf = el.attribute("confidence").toDouble(&ok);
  if (ok) {
    d.confidence = conf;
  }
  d.source = deskewSourceFromString(el.attribute("angle-source"));
  const QStringList uncertain = el.attribute("uncertain").split(' ', Qt::SkipEmptyParts);
  d.angleUncertain = uncertain.contains("angle");
  d.obliqueUncertain = uncertain.contains("oblique");
  const double rejected = el.attribute("rejected-oblique").toDouble(&ok);
  if (ok && d.obliqueUncertain) {
    d.rejectedOblique = rejected;
  }
  return d;
}

}  // namespace

QString deskewSourceToString(const DeskewSource source) {
  switch (source) {
    case DeskewSource::FALLBACK:
      return "fallback";
    case DeskewSource::BEST_GUESS:
      return "best-guess";
    case DeskewSource::NO_TEXT:
      return "no-text";
    case DeskewSource::DISABLED:
      return "low-confidence";
    case DeskewSource::PRIMARY:
      break;
  }
  return QString();
}

DeskewSource deskewSourceFromString(const QString& str) {
  if (str == "fallback") {
    return DeskewSource::FALLBACK;
  }
  if (str == "best-guess") {
    return DeskewSource::BEST_GUESS;
  }
  if (str == "no-text") {
    return DeskewSource::NO_TEXT;
  }
  if (str == "low-confidence") {
    return DeskewSource::DISABLED;
  }
  return DeskewSource::PRIMARY;
}

Params::Params(const double deskewAngleDeg, const Dependencies& deps, const AutoManualMode deskewMode)
    : m_rotation{deskewAngleDeg, deskewMode}, m_oblique{0.0, MODE_AUTO}, m_deps(deps) {}

Params::Params(const double deskewAngleDeg,
               const double obliqueDeg,
               const Dependencies& deps,
               const AutoManualMode deskewMode,
               const AutoManualMode obliqueMode)
    : m_rotation{deskewAngleDeg, deskewMode}, m_oblique{obliqueDeg, obliqueMode}, m_deps(deps) {}

Params::Params(const double deskewAngleDeg,
               const double obliqueDeg,
               const Dependencies& deps,
               const AutoManualMode mode)
    : m_rotation{deskewAngleDeg, mode}, m_oblique{obliqueDeg, mode}, m_deps(deps) {}

Params::Params(const QDomElement& deskewEl)
    : m_rotation{deskewEl.attribute("angle").toDouble(),
                 deskewEl.attribute("mode") == "manual" ? MODE_MANUAL : MODE_AUTO},
      m_oblique{deskewEl.attribute("oblique").toDouble(), obliqueModeFromXml(deskewEl)},
      m_deps(deskewEl.namedItem("dependencies").toElement()),
      m_diag(diagnosticsFromXml(deskewEl)) {}

Params::~Params() = default;

QDomElement Params::toXml(QDomDocument& doc, const QString& name) const {
  QDomElement el(doc.createElement(name));
  el.setAttribute("mode", m_rotation.mode == MODE_AUTO ? "auto" : "manual");
  el.setAttribute("angle", Utils::doubleToString(m_rotation.angle));
  el.setAttribute("oblique", Utils::doubleToString(m_oblique.obliqueAngle));
  el.setAttribute("oblique-mode", m_oblique.mode == MODE_AUTO ? "auto" : "manual");
  // Diagnostics of automatic angles only (issue #15); absent for manual angles and old projects.
  if (const auto conf = confidence()) {
    el.setAttribute("confidence", Utils::doubleToString(std::round(*conf * 1000.0) / 1000.0));
  }
  const QString source = deskewSourceToString(angleSource());
  if (!source.isEmpty()) {
    el.setAttribute("angle-source", source);
  }
  QStringList uncertain;
  if (isAngleUncertain()) {
    uncertain << "angle";
  }
  if (isObliqueUncertain()) {
    uncertain << "oblique";
    if (m_diag.rejectedOblique) {
      el.setAttribute("rejected-oblique", Utils::doubleToString(std::round(*m_diag.rejectedOblique * 1000.0) / 1000.0));
    }
  }
  if (!uncertain.isEmpty()) {
    el.setAttribute("uncertain", uncertain.join(' '));
  }
  el.appendChild(m_deps.toXml(doc, "dependencies"));
  return el;
}
}  // namespace deskew