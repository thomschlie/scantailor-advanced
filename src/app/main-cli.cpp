// Copyright (C) 2026  scantailor-cli contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.
//
// scantailor-cli — headless batch processing for ScanTailor Advanced.
//
//   scantailor-cli [options] -o <outdir> <image>...        new project from images
//   scantailor-cli [options] --project <file> [-o <outdir>] existing project
//
// Stages: 1 fix orientation, 2 split pages, 3 deskew, 4 select content, 5 page layout, 6 output.

#include <config.h>
#include <core/Application.h>
#include <core/DefaultParams.h>
#include <core/DefaultParamsProfileManager.h>
#include <core/DefaultParamsProvider.h>
#include "filters/output/ColorParams.h"
#include "filters/select_content/LayoutDetector.h"

#include "Dpi.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QThread>
#include <iostream>

#include "ConsoleBatch.h"

namespace {
int fail(const QString& msg) {
  std::cerr << "scantailor-cli: " << msg.toStdString() << std::endl;
  return 2;
}

bool parseStage(const QCommandLineParser& p, const QString& name, int fallback, int* out) {
  if (!p.isSet(name)) {
    *out = fallback - 1;
    return true;
  }
  bool ok = false;
  const int v = p.value(name).toInt(&ok);
  if (!ok || v < 1 || v > 6) {
    return false;
  }
  *out = v - 1;
  return true;
}
}  // namespace

int main(int argc, char* argv[]) {
  // No window system needed: default to the offscreen platform unless the caller chose one.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  }
  Application app(argc, argv);
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
      "Stages: 1 fix orientation, 2 split pages, 3 deskew, 4 select content, 5 page layout, 6 output.\n"
      "Pages that already have settings in a project (e.g. fixed by hand in the GUI) keep them;\n"
      "the default parameters apply only to pages without settings.");
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
               "marginalia go into the report. Default: $SCANTAILOR_LAYOUT_MODEL. Content boxes change only "
               "with --include (or toggles stored in the project).",
               "file"});
  p.addOption({"layout-model-sha256",
               "Refuse the layout model unless its sha256 is this. Default: $SCANTAILOR_LAYOUT_MODEL_SHA256.", "hex"});
  p.addOption({"include",
               "Layout toggles of stage 4, set on every page (comma separated): page-numbers, tabs, headers, "
               "marginalia take those elements into the automatic content box; clean-edge cuts the paper edge "
               "off it; all; none. Needs a layout model and stage 4 in the range. Default: each page keeps "
               "its own (off unless set in the GUI). Boxes set by hand are never changed.",
               "list"});
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

  ConsoleBatch::Options opt;
  if (!parseStage(p, "start-stage", 1, &opt.startFilter) || !parseStage(p, "end-stage", 6, &opt.endFilter)) {
    return fail("stages are numbered 1-6");
  }
  opt.threads = p.isSet("threads") ? p.value("threads").toInt() : QThread::idealThreadCount();
  if (opt.threads < 1) {
    return fail("--threads must be at least 1");
  }
  opt.verbose = p.isSet("verbose");

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
      if (opt.verbose) {
        std::cout << "layout model: " << model.toStdString() << " (sha256 "
                  << select_content::LayoutDetector::instance()->modelSha256().toStdString() << ")" << std::endl;
      }
    } else if (p.isSet("layout-model-sha256")) {
      return fail("--layout-model-sha256 needs --layout-model");
    }
    if (p.isSet("include")) {
      QString error;
      const unsigned flags = select_content::layout::parseIncludeList(p.value("include"), &error);
      if (!error.isEmpty()) {
        return fail("--include: " + error);
      }
      if (flags != 0 && model.isEmpty()) {
        return fail("--include needs a layout model (--layout-model or $SCANTAILOR_LAYOUT_MODEL)");
      }
      if (opt.startFilter > 3 || opt.endFilter < 3) {
        return fail("--include needs stage 4 (select content) in the stage range");
      }
      opt.layoutInclude = static_cast<int>(flags);
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
        const QString m = p.value("color-mode");
        output::ColorParams colorParams = out.getColorParams();
        if (m == "bw") {
          colorParams.setColorMode(output::BLACK_AND_WHITE);
        } else if (m == "color") {
          colorParams.setColorMode(output::COLOR_GRAYSCALE);
        } else if (m == "mixed") {
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
    std::unique_ptr<ConsoleBatch> batch;
    QString saveTo = p.value("save-project");
    if (!project.isEmpty()) {
      batch = std::make_unique<ConsoleBatch>(project, p.value("output-dir"));
      if (saveTo.isEmpty()) {
        saveTo = project;
      }
    } else {
      const int dpi = p.isSet("source-dpi") ? p.value("source-dpi").toInt() : 0;
      if (p.isSet("source-dpi") && dpi <= 0) {
        return fail("--source-dpi must be positive");
      }
      batch = std::make_unique<ConsoleBatch>(images, p.value("output-dir"),
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
      if (opt.verbose) {
        std::cout << "project: " << saveTo.toStdString() << std::endl;
      }
    }
    if (p.isSet("report")) {
      if (!batch->writeReport(p.value("report"))) {
        return fail("cannot write report: " + p.value("report"));
      }
    }
    if (failed > 0) {
      std::cerr << "scantailor-cli: " << failed << " page task(s) failed" << std::endl;
      return 1;
    }
  } catch (const std::exception& e) {
    return fail(QString::fromStdString(e.what()));
  }
  return 0;
}
