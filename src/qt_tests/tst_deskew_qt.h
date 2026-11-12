// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef TST_DESKEW_QT_H
#define TST_DESKEW_QT_H

#include <QObject>

// Deskew stage GUI of the fallback (scantailor-cli issue #15). Own file: deskew's and
// select_content's OptionsWidget.h both include a generated "ui_OptionsWidget.h".
class TestDeskewQt : public QObject {
  Q_OBJECT
 private slots:
  void testUncertainShownAndFallbackSwitch();
};

#endif
