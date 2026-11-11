// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "BatchReport.h"

#include <core/BatchProcessor.h>

#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <map>
#include <vector>

#include "PageInfo.h"
#include "PageSequence.h"
#include "ProjectPages.h"
#include "StageSequence.h"
#include "filters/select_content/Filter.h"
#include "filters/select_content/LayoutDetector.h"

namespace {
QJsonObject attributesOf(const QDomElement& el) {
  QJsonObject obj;
  const QDomNamedNodeMap attrs = el.attributes();
  for (int i = 0; i < attrs.count(); ++i) {
    const QDomAttr a = attrs.item(i).toAttr();
    obj.insert(a.name(), a.value());
  }
  return obj;
}

// Generic dump of one <page> (or <image>) settings element: its child elements with their
// attributes, nested one level (e.g. <params mode="auto"><content-rect .../></params>).
QJsonObject dumpSettings(const QDomElement& pageEl) {
  QJsonObject out;
  for (QDomElement c = pageEl.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
    QJsonObject obj = attributesOf(c);
    for (QDomElement cc = c.firstChildElement(); !cc.isNull(); cc = cc.nextSiblingElement()) {
      obj.insert(cc.tagName(), attributesOf(cc));
    }
    out.insert(c.tagName(), obj);
  }
  return out;
}
}  // namespace

bool writeBatchReport(const BatchProcessor& batch, const QString& reportFile) {
  // Build the report from the project XML the GUI would save: the filter settings are
  // private to the filters, but every filter serialises them there.
  QTemporaryFile tmp;
  if (!tmp.open()) {
    return false;
  }
  tmp.close();
  if (!batch.saveProject(tmp.fileName())) {
    return false;
  }
  QFile f(tmp.fileName());
  QDomDocument doc;
  if (!f.open(QIODevice::ReadOnly) || !doc.setContent(&f)) {
    return false;
  }
  const QDomElement root = doc.documentElement();

  // page id -> {image file, sub page}; image id -> file name
  std::map<QString, QString> fileById, dirById, imageFile;
  for (QDomElement d = root.firstChildElement("directories").firstChildElement("directory"); !d.isNull();
       d = d.nextSiblingElement("directory")) {
    dirById[d.attribute("id")] = d.attribute("path");
  }
  for (QDomElement e = root.firstChildElement("files").firstChildElement("file"); !e.isNull();
       e = e.nextSiblingElement("file")) {
    fileById[e.attribute("id")] = dirById[e.attribute("dirId")] + "/" + e.attribute("name");
  }
  for (QDomElement e = root.firstChildElement("images").firstChildElement("image"); !e.isNull();
       e = e.nextSiblingElement("image")) {
    imageFile[e.attribute("id")] = fileById[e.attribute("fileId")];
  }

  struct PageEntry {
    QJsonObject obj;
    QJsonObject stages;
  };
  std::map<QString, PageEntry> pages;  // by page id
  std::vector<QString> order;
  std::map<QString, QString> pageImage;
  for (QDomElement p = root.firstChildElement("pages").firstChildElement("page"); !p.isNull();
       p = p.nextSiblingElement("page")) {
    const QString id = p.attribute("id");
    PageEntry entry;
    entry.obj.insert("id", id);
    entry.obj.insert("image", imageFile[p.attribute("imageId")]);
    entry.obj.insert("subPage", p.attribute("subPage"));
    pages[id] = entry;
    pageImage[id] = p.attribute("imageId");
    order.push_back(id);
  }

  // Output file of each page, matched by (source image, sub page).
  std::map<QString, QString> outputByKey;
  for (const PageInfo& page : batch.pages()->toPageSequence(PAGE_VIEW)) {
    outputByKey[page.imageId().filePath() + "|" + page.id().subPageAsString()]
        = batch.outputFileNameGenerator().filePathFor(page.id());
  }
  // Layout detector results (only with a model; kept in memory, not in the project).
  std::map<QString, QJsonObject> layoutByKey;
  for (const PageInfo& page : batch.pages()->toPageSequence(PAGE_VIEW)) {
    if (const auto layout = batch.stages()->selectContentFilter()->layoutResult(page.id())) {
      layoutByKey[page.imageId().filePath() + "|" + page.id().subPageAsString()] = layout->toJson();
    }
  }
  for (const QString& id : order) {
    QJsonObject& obj = pages[id].obj;
    const QString key = obj.value("image").toString() + "|" + obj.value("subPage").toString();
    const auto it = outputByKey.find(key);
    if (it != outputByKey.end()) {
      obj.insert("output", it->second);
    }
    const auto lt = layoutByKey.find(key);
    if (lt != layoutByKey.end()) {
      obj.insert("layout", lt->second);
    }
  }

  const QDomElement filters = root.firstChildElement("filters");
  for (QDomElement fl = filters.firstChildElement(); !fl.isNull(); fl = fl.nextSiblingElement()) {
    const QString stage = fl.tagName();
    for (QDomElement p = fl.firstChildElement(); !p.isNull(); p = p.nextSiblingElement()) {
      if (p.tagName() == "page" && pages.count(p.attribute("id"))) {
        PageEntry& e = pages[p.attribute("id")];
        e.stages.insert(stage, dumpSettings(p));
      } else if (p.tagName() == "image") {
        // fix_orientation and page_split store settings per image, not per page.
        for (const QString& id : order) {
          if (pageImage[id] == p.attribute("id")) {
            pages[id].stages.insert(stage, dumpSettings(p));
          }
        }
      }
    }
  }

  QJsonArray arr;
  for (const QString& id : order) {
    QJsonObject obj = pages[id].obj;
    obj.insert("stages", pages[id].stages);
    arr.append(obj);
  }
  QJsonObject top;
  top.insert("outputDirectory", batch.outputFileNameGenerator().outDir());
  if (const auto detector = select_content::LayoutDetector::instance()) {
    QJsonObject model;
    model.insert("path", detector->modelPath());
    model.insert("sha256", detector->modelSha256());
    model.insert("scoreThreshold", select_content::LayoutDetector::kScoreThreshold);
    top.insert("layoutModel", model);
  }
  top.insert("pages", arr);

  QFile out(reportFile);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  out.write(QJsonDocument(top).toJson(QJsonDocument::Indented));
  return true;
}
