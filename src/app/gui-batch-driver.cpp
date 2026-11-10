// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// scantailor-gui-batch — drives the real GUI (MainWindow) the way a user does, for the
// acceptance test "CLI output == GUI output":
//
//   scantailor-gui-batch <project.ScanTailor> <stage> [<stage>...]
//
// Opens the project, then for each stage (1-6): selects it in the stage list (as a click
// does: filterSelectionChanged, incl. the orientation/content-box propagation) and starts
// batch processing through the stage list's launchBatchProcessing() signal (what the ▶
// button emits), waits until the batch is finished, and finally saves the project
// (File > Save Project). Runs on the offscreen platform unless QT_QPA_PLATFORM is set.
// Test helper only; not installed.

#include <config.h>
#include <core/Application.h>
#include <core/ApplicationSettings.h>
#include <core/ColorSchemeFactory.h>
#include <core/ColorSchemeManager.h>
#include <core/DefaultParams.h>
#include <core/DefaultParamsProfileManager.h>
#include <core/DefaultParamsProvider.h>
#include <core/IconProvider.h>
#include <core/StyledIconPack.h>

#include <QAction>
#include <QElapsedTimer>
#include <QSettings>
#include <QTableView>
#include <QTest>
#include <iostream>

#include "MainWindow.h"

namespace {
int fail(const QString& msg) {
  std::cerr << "scantailor-gui-batch: " << msg.toStdString() << std::endl;
  return 2;
}

// Process events until pred() holds; false on timeout.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
  QElapsedTimer t;
  t.start();
  while (!pred()) {
    if (t.elapsed() > timeoutMs) {
      return false;
    }
    QTest::qWait(50);
  }
  return true;
}
}  // namespace

int main(int argc, char* argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  }
  Application app(argc, argv);
  Application::setApplicationName(APPLICATION_NAME);
  Application::setOrganizationName(ORGANIZATION_NAME);
  QSettings::setDefaultFormat(QSettings::IniFormat);
  if (app.isPortableVersion()) {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.getPortableConfigPath());
  }

  const QStringList args = Application::arguments();
  if (args.size() < 3) {
    return fail("usage: scantailor-gui-batch <project.ScanTailor> <stage 1-6>...");
  }
  const QString projectFile = args.at(1);
  std::vector<int> stages;
  for (int i = 2; i < args.size(); ++i) {
    bool ok = false;
    const int s = args.at(i).toInt(&ok);
    if (!ok || s < 1 || s > 6) {
      return fail("stages are numbered 1-6: " + args.at(i));
    }
    stages.push_back(s - 1);
  }

  // As main.cpp, so the window builds the same way.
  {
    std::unique_ptr<ColorScheme> scheme
        = ColorSchemeFactory().create(ApplicationSettings::getInstance().getColorScheme());
    ColorSchemeManager::instance().setColorScheme(*scheme);
  }
  IconProvider::getInstance().setIconPack(StyledIconPack::createDefault());
  // Same default parameters as scantailor-cli without --profile.
  DefaultParamsProvider::getInstance().setParams(DefaultParamsProfileManager().createDefaultProfile(), "Default");
  // The user's settings are read as they are (also by scantailor-cli), so both use the
  // same output options; nothing is written to them.

  auto* window = new MainWindow();
  window->show();
  window->openProject(projectFile);

  auto* filterList = window->findChild<QTableView*>("filterList");
  if (filterList == nullptr) {
    return fail("stage list (filterList) not found");
  }
  if (!waitFor([&] { return filterList->model() && filterList->model()->rowCount() == 6; }, 60000)) {
    return fail("project did not load: " + projectFile);
  }
  QTest::qWait(500);

  for (const int stage : stages) {
    std::cout << "stage " << (stage + 1) << ": select + batch" << std::endl;
    filterList->selectRow(stage);
    QTest::qWait(300);
    // What the stage list's ▶ button does (StageListView: clicked -> launchBatchProcessing).
    if (!QMetaObject::invokeMethod(filterList, "launchBatchProcessing")) {
      return fail("cannot start batch processing");
    }
    // MainWindow disables the stage list while a batch runs and enables it at the end.
    if (!waitFor([&] { return filterList->isEnabled(); }, 60 * 60 * 1000)) {
      return fail("batch processing did not finish");
    }
    QTest::qWait(300);
  }

  auto* save = window->findChild<QAction*>("actionSaveProject");
  if (save == nullptr) {
    return fail("action actionSaveProject not found");
  }
  save->trigger();
  QTest::qWait(500);
  std::cout << "saved " << projectFile.toStdString() << std::endl;
  // Leave without closing the window: closing could ask questions; the project is saved.
  std::cout.flush();
  std::_Exit(0);
}
