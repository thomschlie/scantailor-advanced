// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_BATCHREPORT_H_
#define SCANTAILOR_APP_BATCHREPORT_H_

#include <QString>

class BatchProcessor;

/**
 * Writes a per-page JSON report of a batch run (scantailor-cli --report): for every page its
 * image, output file and the settings of every stage, built from the project as it would be
 * saved now.
 */
bool writeBatchReport(const BatchProcessor& batch, const QString& reportFile);

#endif  // SCANTAILOR_APP_BATCHREPORT_H_
