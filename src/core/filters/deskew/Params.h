// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// Split Params (Issue #1): refactor into RotationParams and ObliqueParams as requested by Zvezdochiot;
// in the dev branch this is implemented (single <params> XML preserved for compatibility).

#ifndef SCANTAILOR_DESKEW_PARAMS_H_
#define SCANTAILOR_DESKEW_PARAMS_H_

#include <QString>
#include <algorithm>
#include <cmath>
#include <optional>

#include "AutoManualMode.h"
#include "Dependencies.h"
#include "SkewFallback.h"

class QDomDocument;
class QDomElement;

namespace deskew {

/** Rotation (deskew) angle and auto/manual mode. */
struct RotationParams {
  double angle = 0.0;
  AutoManualMode mode = MODE_AUTO;
};

/** Oblique (shear) angle in degrees and its own auto/manual mode (issue #117). */
struct ObliqueParams {
  double obliqueAngle = 0.0;
  AutoManualMode mode = MODE_AUTO;
};

/**
 * How an automatic deskew / oblique angle was found (scantailor-cli issue #15). Only meaningful
 * while the respective mode is automatic; a manually set angle never carries these.
 * In the project XML: attributes "confidence", "angle-source", "uncertain", "rejected-oblique" of
 * <params>; missing
 * attributes (projects of older versions) mean "unknown".
 */
struct AutoDiagnostics {
  /** SkewFinder's confidence; empty = unknown (older project, or not computed). */
  std::optional<double> confidence;
  DeskewSource source = DeskewSource::PRIMARY;
  /** No confident estimate: the angle is a best guess. */
  bool angleUncertain = false;
  /** The automatic oblique was implausibly large (> MAX_PLAUSIBLE_OBLIQUE) and is set to 0. */
  bool obliqueUncertain = false;
  /** That rejected automatic oblique (degrees), if any. */
  std::optional<double> rejectedOblique;
};

class Params {
 public:
  // Member-wise copying is OK.

  Params(double deskewAngleDeg, const Dependencies& deps, AutoManualMode deskewMode);

  Params(double deskewAngleDeg,
         double obliqueDeg,
         const Dependencies& deps,
         AutoManualMode deskewMode,
         AutoManualMode obliqueMode);

  /** Legacy: oblique mode matches \p mode (coupled deskew/oblique). */
  Params(double deskewAngleDeg, double obliqueDeg, const Dependencies& deps, AutoManualMode mode);

  explicit Params(const QDomElement& deskewEl);

  ~Params();

  double deskewAngle() const;

  double obliqueAngle() const;

  const Dependencies& dependencies() const;

  AutoManualMode mode() const;

  AutoManualMode obliqueMode() const;

  /** Legacy (#114): true when oblique runs in automatic mode. */
  bool autoOblique() const;

  QDomElement toXml(QDomDocument& doc, const QString& name) const;

  /** Diagnostics as stored (see AutoDiagnostics); use the accessors below for the effective state. */
  const AutoDiagnostics& diagnostics() const { return m_diag; }

  void setDiagnostics(const AutoDiagnostics& diag) { m_diag = diag; }

  /** SkewFinder's confidence of an automatic angle, if known. */
  std::optional<double> confidence() const {
    return m_rotation.mode == MODE_AUTO ? m_diag.confidence : std::nullopt;
  }

  /** Where an automatic angle came from (PRIMARY for manual angles). */
  DeskewSource angleSource() const { return m_rotation.mode == MODE_AUTO ? m_diag.source : DeskewSource::PRIMARY; }

  bool isAngleUncertain() const { return m_rotation.mode == MODE_AUTO && m_diag.angleUncertain; }

  bool isObliqueUncertain() const { return m_oblique.mode == MODE_AUTO && m_diag.obliqueUncertain; }

  bool isUncertain() const { return isAngleUncertain() || isObliqueUncertain(); }

 private:
  RotationParams m_rotation;
  ObliqueParams m_oblique;
  Dependencies m_deps;
  AutoDiagnostics m_diag;
};

/** XML value of DeskewSource ("" for PRIMARY), and back (unknown -> PRIMARY). */
QString deskewSourceToString(DeskewSource source);

DeskewSource deskewSourceFromString(const QString& str);


inline double Params::deskewAngle() const {
  return m_rotation.angle;
}

inline double Params::obliqueAngle() const {
  return m_oblique.obliqueAngle;
}

inline const Dependencies& Params::dependencies() const {
  return m_deps;
}

inline AutoManualMode Params::mode() const {
  return m_rotation.mode;
}

inline AutoManualMode Params::obliqueMode() const {
  return m_oblique.mode;
}

inline bool Params::autoOblique() const {
  return m_oblique.mode == MODE_AUTO;
}
}  // namespace deskew

#endif  // ifndef SCANTAILOR_DESKEW_PARAMS_H_
