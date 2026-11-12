// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "tst_core_qt.h"
#include "tst_deskew_qt.h"
#include "tst_ux_qt.h"

#include <core/Application.h>

#include <QtTest/QtTest>

int main(int argc, char* argv[]) {
  // ScanTailor's Application, not a plain QApplication: DefaultParamsProfileManager (reached from
  // the deskew options widget through DefaultParamsProvider) casts qApp to it.
  Application app(argc, argv);
  int status = 0;

  TestCoreQt tc1;
  status |= QTest::qExec(&tc1, argc, argv);

  TestUxQt tc2;
  status |= QTest::qExec(&tc2, argc, argv);

  TestDeskewQt tc3;
  status |= QTest::qExec(&tc3, argc, argv);

  return status;
}
