// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// scantailor-cli: headless batch processing for ScanTailor Advanced.
//
//   scantailor-cli [options] -o <outdir> <image>...        new project from images
//   scantailor-cli [options] --project <file> [-o <outdir>] existing project
//
// Stages: 1 fix orientation, 2 split pages, 3 deskew, 4 select content, 5 margins, 6 output.

#include <config.h>
#include <core/Application.h>
#include <core/BatchProcessor.h>
#include <core/DefaultParams.h>
#include <core/DefaultParamsProfileManager.h>
#include <core/DefaultParamsProvider.h>

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QThread>
#include <cstdlib>
#include <iostream>

#include "BatchReport.h"
#include "Dpi.h"
#include "PageInfo.h"
#include "filters/output/ColorParams.h"
#include "filters/select_content/LayoutDetector.h"

namespace {
// Exit codes.
const int kExitOk = 0;
const int kExitPagesFailed = 1;  // processing ran, but some pages failed
const int kExitError = 2;        // bad options, unreadable input, cannot write the project

const char* const kStageNames[]
    = {"fix_orientation", "page_split", "deskew", "select_content", "page_layout", "output"};

int fail(const QString& msg) {
  std::cerr << "scantailor-cli: " << msg.toStdString() << std::endl;
  return kExitError;
}

bool parseStage(const QCommandLineParser& parser, const QString& name, const int fallback, int* out) {
  if (!parser.isSet(name)) {
    *out = fallback - 1;
    return true;
  }
  bool ok = false;
  const int value = parser.value(name).toInt(&ok);
  if (!ok || (value < 1) || (value > 6)) {
    return false;
  }
  *out = value - 1;
  return true;
}

// Qt aborts (qFatal) when the platform plugin cannot be loaded, with a message aimed at GUI
// users. While the application object is being created, turn that into a clear error.
QtMessageHandler previousMessageHandler = nullptr;

void platformPluginMessageHandler(const QtMsgType type, const QMessageLogContext& context, const QString& msg) {
  if (type == QtFatalMsg) {
    const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
    std::cerr << "scantailor-cli: cannot load the Qt platform plugin \"" << platform.constData() << "\".\n"
              << "scantailor-cli needs no display, but it needs a Qt platform plugin: by default \"offscreen\"\n"
              << "(part of Qt's base package), or the one QT_QPA_PLATFORM names (e.g. xcb, wayland).\n"
              << "Qt said: " << msg.toStdString() << std::endl;
    std::_Exit(kExitError);
  }
  if (previousMessageHandler) {
    previousMessageHandler(type, context, msg);
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  // No window system needed: default to the offscreen platform unless the caller chose one.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  }
  previousMessageHandler = qInstallMessageHandler(platformPluginMessageHandler);
  Application app(argc, argv);
  qInstallMessageHandler(previousMessageHandler);

#ifdef _WIN32
  // As the GUI: get rid of all references to Qt's installation directory.
  Application::setLibraryPaths(QStringList(Application::applicationDirPath()));
#endif

  // Same names and settings format as the GUI, so profiles and settings are shared.
  Application::setApplicationName(APPLICATION_NAME);
  Application::setOrganizationName(ORGANIZATION_NAME);
  QSettings::setDefaultFormat(QSettings::IniFormat);
  if (app.isPortableVersion()) {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.getPortableConfigPath());
  }

  QCommandLineParser p;
  p.setApplicationDescription(
      "Headless batch processing for ScanTailor Advanced.\n"
      "Stages: 1 fix orientation, 2 split pages, 3 deskew, 4 select content, 5 margins, 6 output.\n"
      "Pages that already have settings in a project (e.g. fixed by hand in the GUI) keep them;\n"
      "the default parameters apply only to pages without settings.\n"
      "No display is needed: Qt's \"offscreen\" platform plugin is used unless QT_QPA_PLATFORM is set.\n"
      "Exit status: 0 all pages processed, 1 some pages failed, 2 error (options, input, project file).");
  p.addHelpOption();
  p.addPositionalArgument("images", "Input images (new project). Not with --project.", "[image...]");
  p.addOption({"project", "Process an existing .ScanTailor project.", "file"});
  p.addOption({{"o", "output-dir"}, "Output directory (required for images; overrides the project's).", "dir"});
  p.addOption({"save-project",
               "Write the project here after processing. Default: back to --project, or "
               "<output-dir>/project.ScanTailor for images.",
               "file"});
  p.addOption({"no-save", "Do not write a project file."});
  p.addOption({"profile",
               "Default parameters: a profile name saved in the GUI (\"Default\", \"Source\" or a user "
               "profile) or a path to a .stp file. Default: \"Default\".",
               "name|file"});
  p.addOption({"color-mode",
               "Output mode for pages without settings: bw, color (colour/greyscale) or mixed. "
               "Default: from the profile (Default: bw).",
               "mode"});
  p.addOption({"output-dpi", "Output resolution for pages without settings. Default: from the profile.", "dpi"});
  p.addOption({"start-stage", "First stage to run (1-6). Default 1.", "n"});
  p.addOption({"end-stage", "Last stage to run (1-6). Default 6.", "n"});
  p.addOption({"source-dpi", "Resolution of the input images, overriding the files (images mode).", "dpi"});
  p.addOption({"rtl", "Right-to-left page order (images mode)."});
  p.addOption({{"j", "threads"}, "Parallel pages per stage. Default: number of CPUs.", "n"});
  p.addOption({"report", "Write a per-page JSON report (settings of every stage).", "file"});
  p.addOption({"layout-model",
               "Layout model (ONNX) for the select-content stage: detected page numbers, tabs, headers and "
               "marginalia go into the report. Default: $SCANTAILOR_LAYOUT_MODEL. Content boxes are not changed.",
               "file"});
  p.addOption({"layout-model-sha256",
               "Refuse the layout model unless its sha256 is this. Default: $SCANTAILOR_LAYOUT_MODEL_SHA256.", "hex"});
  p.addOption({{"v", "verbose"}, "Print stages and pages."});
  p.process(app);

  const QStringList images = p.positionalArguments();
  const QString project = p.value("project");
  if (project.isEmpty() == images.isEmpty()) {
    return fail("give either --project <file> or input images (see --help)");
  }
  if (!images.isEmpty() && !p.isSet("output-dir")) {
    return fail("--output-dir is required with input images");
  }

  BatchProcessor::Options opt;
  if (!parseStage(p, "start-stage", 1, &opt.startFilter) || !parseStage(p, "end-stage", 6, &opt.endFilter)) {
    return fail("stages are numbered 1-6");
  }
  opt.threads = p.isSet("threads") ? p.value("threads").toInt() : QThread::idealThreadCount();
  if (opt.threads < 1) {
    return fail("--threads must be at least 1");
  }
  const bool verbose = p.isSet("verbose");
  if (verbose) {
    opt.stageStarted = [](const int stage, const int numPages) {
      std::cout << "stage " << (stage + 1) << " " << kStageNames[stage] << ": " << numPages << " pages" << std::endl;
    };
    opt.pageProcessed
        = [](const PageInfo& page) { std::cout << "  " << page.imageId().filePath().toStdString() << std::endl; };
  }
  opt.pageFailed = [](const PageInfo& page, const QString& error) {
    std::cerr << "error: " << page.imageId().filePath().toStdString() << ": " << error.toStdString() << std::endl;
  };

  // Layout detector: load the model now, so a bad path or hash fails before any processing.
  {
    const QString model
        = p.isSet("layout-model") ? p.value("layout-model") : qEnvironmentVariable("SCANTAILOR_LAYOUT_MODEL");
    const QString sha = p.isSet("layout-model-sha256") ? p.value("layout-model-sha256")
                                                       : qEnvironmentVariable("SCANTAILOR_LAYOUT_MODEL_SHA256");
    if (!model.isEmpty()) {
      const QString error = select_content::LayoutDetector::configure(model, sha);
      if (!error.isEmpty()) {
        return fail(error);
      }
      if (verbose) {
        std::cout << "layout model: " << model.toStdString() << " (sha256 "
                  << select_content::LayoutDetector::instance()->modelSha256().toStdString() << ")" << std::endl;
      }
    } else if (p.isSet("layout-model-sha256")) {
      return fail("--layout-model-sha256 needs --layout-model");
    }
  }

  // Default parameters for pages without settings.
  {
    const QString profile = p.isSet("profile") ? p.value("profile") : QStringLiteral("Default");
    std::unique_ptr<DefaultParams> params;
    QString name = profile;
    if (profile == "Default") {
      params = DefaultParamsProfileManager().createDefaultProfile();
    } else if (profile == "Source") {
      params = DefaultParamsProfileManager().createSourceProfile();
    } else if (profile.endsWith(".stp") || profile.contains('/')) {
      const QFileInfo fi(profile);
      name = fi.completeBaseName();
      params = DefaultParamsProfileManager(fi.absolutePath()).readProfile(name);
    } else {
      params = DefaultParamsProfileManager().readProfile(profile);
    }
    if (!params) {
      return fail("cannot read profile: " + profile);
    }
    // Output options on top of the profile (again only for pages without settings).
    if (p.isSet("color-mode") || p.isSet("output-dpi")) {
      DefaultParams::OutputParams out = params->getOutputParams();
      if (p.isSet("color-mode")) {
        const QString mode = p.value("color-mode");
        output::ColorParams colorParams = out.getColorParams();
        if (mode == "bw") {
          colorParams.setColorMode(output::BLACK_AND_WHITE);
        } else if (mode == "color") {
          colorParams.setColorMode(output::COLOR_GRAYSCALE);
        } else if (mode == "mixed") {
          colorParams.setColorMode(output::MIXED);
        } else {
          return fail("--color-mode is bw, color or mixed");
        }
        out.setColorParams(colorParams);
      }
      if (p.isSet("output-dpi")) {
        const int dpi = p.value("output-dpi").toInt();
        if (dpi <= 0) {
          return fail("--output-dpi must be positive");
        }
        out.setDpi(Dpi(dpi, dpi));
      }
      params->setOutputParams(out);
      name += "+cli";
    }
    DefaultParamsProvider::getInstance().setParams(std::move(params), name);
  }

  try {
    std::unique_ptr<BatchProcessor> batch;
    QString saveTo = p.value("save-project");
    if (!project.isEmpty()) {
      batch = std::make_unique<BatchProcessor>(project, p.value("output-dir"));
      if (saveTo.isEmpty()) {
        saveTo = project;
      }
    } else {
      const int dpi = p.isSet("source-dpi") ? p.value("source-dpi").toInt() : 0;
      if (p.isSet("source-dpi") && (dpi <= 0)) {
        return fail("--source-dpi must be positive");
      }
      batch = std::make_unique<BatchProcessor>(images, p.value("output-dir"),
                                               p.isSet("rtl") ? Qt::RightToLeft : Qt::LeftToRight, dpi);
      if (saveTo.isEmpty()) {
        saveTo = QDir(p.value("output-dir")).filePath("project.ScanTailor");
      }
    }

    const int failed = batch->process(opt);

    if (!p.isSet("no-save")) {
      if (!batch->saveProject(saveTo)) {
        return fail("cannot write project: " + saveTo);
      }
      if (verbose) {
        std::cout << "project: " << saveTo.toStdString() << std::endl;
      }
    }
    if (p.isSet("report")) {
      if (!writeBatchReport(*batch, p.value("report"))) {
        return fail("cannot write report: " + p.value("report"));
      }
    }
    if (failed > 0) {
      std::cerr << "scantailor-cli: " << failed << " page task(s) failed" << std::endl;
      return kExitPagesFailed;
    }
  } catch (const std::exception& e) {
    return fail(QString::fromStdString(e.what()));
  }
  return kExitOk;
}
