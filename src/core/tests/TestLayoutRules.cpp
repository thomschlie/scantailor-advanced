// Copyright (C) 2026  Thomas Schlieter
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include <filters/select_content/LayoutImage.h>
#include <filters/select_content/LayoutRules.h>
#include <filters/select_content/Params.h>

#include <QDomDocument>

#include <boost/test/unit_test.hpp>
#include <cmath>

namespace Tests {
using namespace select_content::layout;

namespace {
constexpr double kDpi = 300;
constexpr double kPxPerMm = kDpi / 25.4;

void fillRect(Plane8& img, int x0, int y0, int x1, int y1, uint8_t v) {
  for (int y = std::max(0, y0); y < std::min(img.height, y1); ++y) {
    for (int x = std::max(0, x0); x < std::min(img.width, x1); ++x) {
      img.row(y)[x] = v;
    }
  }
}

// A white page (A5-ish, 300 dpi) with a black thumb-index tab at the right edge: 7 x 25 mm,
// white digits inside, 1 mm from the paper edge.
Plane8 pageWithTab(Box* tab) {
  Plane8 img(1200, 1800, 1, 235);
  const int x1 = 1200 - static_cast<int>(kPxPerMm);
  const int x0 = x1 - static_cast<int>(7 * kPxPerMm);
  const int y0 = 600;
  const int y1 = y0 + static_cast<int>(25 * kPxPerMm);
  fillRect(img, x0, y0, x1, y1, 20);
  fillRect(img, x0 + 20, y0 + 100, x0 + 30, y0 + 130, 235);  // "1" (2.5 mm: closed vertically)
  fillRect(img, x0 + 45, y0 + 100, x0 + 60, y0 + 110, 235);  // part of a "7"
  *tab = Box{double(x0), double(y0), double(x1), double(y1)};
  return img;
}
}  // namespace

BOOST_AUTO_TEST_SUITE(LayoutRulesTestSuite)

BOOST_AUTO_TEST_CASE(test_longest_run) {
  BOOST_CHECK((longestRun({false, true, true, false, true, true, true}) == std::pair<int, int>(4, 7)));
  BOOST_CHECK((longestRun({true, true, false, true, true}) == std::pair<int, int>(0, 2)));  // first of equals
  BOOST_CHECK((longestRun({false, false, false}) == std::pair<int, int>(0, 3)));             // none: all
}

BOOST_AUTO_TEST_CASE(test_run_through) {
  const std::vector<bool> m{false, true, true, false, false, true, true, true, false};
  BOOST_CHECK((*runThrough(m, 1.5) == std::pair<int, int>(1, 3)));
  BOOST_CHECK((*runThrough(m, 3.5) == std::pair<int, int>(1, 3)));  // tie between 2 and 5: the first
  BOOST_CHECK((*runThrough(m, 4.6) == std::pair<int, int>(5, 8)));
  BOOST_CHECK(!runThrough(std::vector<bool>(5, false), 2.0));
}

BOOST_AUTO_TEST_CASE(test_clip_and_iou) {
  const Box a{0, 0, 10, 10};
  const Box b{5, 5, 20, 20};
  BOOST_CHECK((*clip(a, b) == Box{5, 5, 10, 10}));
  BOOST_CHECK(!clip(a, Box{10, 0, 20, 10}));  // touching only: no area
  BOOST_CHECK_CLOSE(iou(a, b), 25.0 / (100 + 225 - 25), 1e-9);
  BOOST_CHECK_EQUAL(iou(a, Box{20, 20, 30, 30}), 0.0);
}

BOOST_AUTO_TEST_CASE(test_percentile_like_numpy) {
  std::vector<float> v{4, 1, 3, 2};
  BOOST_CHECK_CLOSE(percentile(v, 90), 3.7, 1e-6);
  BOOST_CHECK_CLOSE(percentile(v, 20), 1.6, 1e-6);
  BOOST_CHECK_CLOSE(percentile(v, 50), 2.5, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_gaussian_kernel) {
  const std::vector<double> k = gaussianKernel(7, 1.5);
  double sum = 0;
  for (double v : k) {
    sum += v;
  }
  BOOST_CHECK_CLOSE(sum, 1.0, 1e-9);
  BOOST_CHECK_CLOSE(k[0], k[6], 1e-9);
  BOOST_CHECK(k[3] > k[2] && k[2] > k[1]);
}

BOOST_AUTO_TEST_CASE(test_resize_area_integer_factor_averages) {
  Plane8 src(4, 2, 1);
  const uint8_t vals[8] = {0, 100, 200, 200, 10, 30, 0, 40};
  std::copy(vals, vals + 8, src.data.begin());
  const Plane8 dst = resizeArea(src, 2, 1);
  BOOST_REQUIRE_EQUAL(dst.width, 2);
  BOOST_CHECK_EQUAL(int(dst.at(0, 0)), 35);   // (0+100+10+30)/4
  BOOST_CHECK_EQUAL(int(dst.at(1, 0)), 110);  // (200+200+0+40)/4
}

BOOST_AUTO_TEST_CASE(test_paper_rect_excludes_dark_scanner_background) {
  // 40 mm of near-black scanner background left of the paper, 10 mm right of it.
  Plane8 img(1400, 1800, 1, 230);
  const int left = static_cast<int>(40 * kPxPerMm);
  const int right = 1400 - static_cast<int>(10 * kPxPerMm);
  fillRect(img, 0, 0, left, 1800, 25);
  fillRect(img, right, 0, 1400, 1800, 25);
  const Box p = paperRect(img, kDpi);
  BOOST_CHECK_LE(std::abs(p.x0 - left), 1.5 * kPxPerMm);
  BOOST_CHECK_LE(std::abs(p.x1 - right), 1.5 * kPxPerMm);
  BOOST_CHECK_EQUAL(p.y0, 0);
  BOOST_CHECK_EQUAL(p.y1, 1800);
}

BOOST_AUTO_TEST_CASE(test_paper_rect_white_page_is_whole_page) {
  const Plane8 img(500, 700, 1, 250);
  const Box p = paperRect(img, kDpi);
  BOOST_CHECK((p == Box{0, 0, 500, 700}));
}

BOOST_AUTO_TEST_CASE(test_grow_tab_to_printed_tab) {
  Box tab;
  const Plane8 img = pageWithTab(&tab);
  const Box paper{0, 0, double(img.width), double(img.height)};
  // The model's box ends short of the printed tab on every side.
  const Box model{tab.x0 + 10, tab.y0 + 40, tab.x1 - 8, tab.y1 - 30};
  const std::optional<Box> g = growTab(img, kDpi, model, paper);
  BOOST_REQUIRE(g);
  BOOST_CHECK_LE(std::abs(g->x0 - tab.x0), 3);
  BOOST_CHECK_LE(std::abs(g->x1 - tab.x1), 3);
  BOOST_CHECK_LE(std::abs(g->y0 - tab.y0), 3);
  BOOST_CHECK_LE(std::abs(g->y1 - tab.y1), 3);
}

BOOST_AUTO_TEST_CASE(test_grow_tab_nothing_dark) {
  const Plane8 img(800, 800, 1, 235);
  const Box paper{0, 0, 800, 800};
  BOOST_CHECK(!growTab(img, kDpi, Box{700, 300, 760, 500}, paper));
}

BOOST_AUTO_TEST_CASE(test_outer_ok) {
  const Box paper{0, 0, 1200, 1800};
  BOOST_CHECK(outerOk(Box{1100, 600, 1188, 900}, paper, kDpi));  // 1 mm from the right edge
  BOOST_CHECK(outerOk(Box{20, 600, 100, 900}, paper, kDpi));     // left edge
  BOOST_CHECK(!outerOk(Box{500, 600, 580, 900}, paper, kDpi));   // middle of the page
  BOOST_CHECK(!outerOk(Box{1150, 1790, 1250, 1900}, paper, kDpi));  // centre off the paper
  BOOST_CHECK_CLOSE(outerGapMm(Box{1100, 600, 1188, 900}, paper, kDpi), 12 * 25.4 / kDpi, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_map_class) {
  const double dpi = 600;
  const double mm = dpi / 25.4;
  const Box small{0, 0, 10 * mm, 5 * mm};
  const Box wide{0, 0, 100 * mm, 5 * mm};
  BOOST_CHECK(mapClass("page_number", wide, dpi) == "page_number");
  BOOST_CHECK(mapClass("tab", wide, dpi) == "tab");
  BOOST_CHECK(mapClass("page_header", small, dpi) == "page_number");
  BOOST_CHECK(mapClass("page_header", wide, dpi) == "header");
  BOOST_CHECK(mapClass("page_footer", wide, dpi) == "marginalia");
  BOOST_CHECK(mapClass("marginalia", small, dpi) == "marginalia");
  BOOST_CHECK(mapClass("text", small, dpi).isEmpty());
}

BOOST_AUTO_TEST_CASE(test_tab_rule) {
  const double dpi = 600;
  const double mm = dpi / 25.4;
  const Box page{0, 0, 150 * mm, 210 * mm};
  const Box autoBox{20 * mm, 20 * mm, 130 * mm, 190 * mm};
  const Box sideStrip{140 * mm, 50 * mm, 148 * mm, 80 * mm};
  BOOST_CHECK(tabRule(sideStrip, page, autoBox, dpi));
  BOOST_CHECK(!tabRule(Box{60 * mm, 50 * mm, 68 * mm, 80 * mm}, page, autoBox, dpi));   // not at a side
  BOOST_CHECK(!tabRule(Box{140 * mm, 50 * mm, 148 * mm, 110 * mm}, page, autoBox, dpi));  // 60 mm high
  const Box autoWide{0, 0, 150 * mm, 210 * mm};
  BOOST_CHECK(!tabRule(sideStrip, page, autoWide, dpi));  // centre inside the automatic box
  BOOST_CHECK(tabRule(sideStrip, page, std::nullopt, dpi));
}

BOOST_AUTO_TEST_CASE(test_decode_detections) {
  // 3 queries x 2 classes; boxes cx, cy, w, h in 0..1 on a 100 x 200 image.
  const float logits[] = {3.0f, -5.0f,    // q0: class 0, 0.95
                          2.0f, -5.0f,    // q1: class 0, 0.88, overlaps q0 -> suppressed
                          -5.0f, 1.0f};   // q2: class 1, 0.73, same box as q1 (other class: kept)
  const float boxes[] = {0.5f, 0.5f, 0.2f, 0.1f,  //
                         0.51f, 0.5f, 0.2f, 0.1f,  //
                         0.51f, 0.5f, 0.2f, 0.1f};
  const std::vector<QString> labels{"page_number", "tab"};
  const std::vector<Detection> d = decodeDetections(logits, boxes, 3, 2, labels, 100, 200, 0.3, 0.5);
  BOOST_REQUIRE_EQUAL(d.size(), 2u);
  BOOST_CHECK(d[0].raw == "page_number");
  BOOST_CHECK_CLOSE(d[0].score, 1 / (1 + std::exp(-3.0)), 1e-4);
  BOOST_CHECK_CLOSE(d[0].box.x0, 40.0, 1e-4);
  BOOST_CHECK_CLOSE(d[0].box.y1, 110.0, 1e-4);
  BOOST_CHECK(d[1].raw == "tab");
  // A threshold above every score: nothing.
  BOOST_CHECK(decodeDetections(logits, boxes, 3, 2, labels, 100, 200, 0.99, 0.5).empty());
}

BOOST_AUTO_TEST_CASE(test_select_elements) {
  Box tab;
  const Plane8 img = pageWithTab(&tab);
  const Box paper{0, 0, double(img.width), double(img.height)};
  std::vector<Detection> dets;
  dets.push_back({"tab", "tab", "tab", 0.9, Box{tab.x0 + 10, tab.y0 + 40, tab.x1 - 8, tab.y1 - 30}});
  dets.push_back({"tab", "tab", "tab", 0.5, Box{500, 600, 560, 800}});                  // not at a side: dropped
  dets.push_back({"page_number", "page_number", "page_number", 0.8, Box{560, 1750, 640, 1850}});  // clipped
  dets.push_back({"text", "", "", 0.9, Box{100, 100, 900, 1500}});                       // never an element
  const std::vector<Element> els = selectElements(dets, img, kDpi, paper);
  BOOST_REQUIRE_EQUAL(els.size(), 2u);
  BOOST_CHECK(els[0].type == "tab");
  BOOST_CHECK_LE(std::abs(els[0].box.x1 - tab.x1), 3);
  BOOST_CHECK((els[0].modelBox == dets[0].box));
  BOOST_CHECK(els[1].type == "page_number");
  BOOST_CHECK_EQUAL(els[1].box.y1, 1800);
}

// ---- final box (ml/apply_boxes.py): automatic box U included elements, paper-edge cut ----
namespace {
constexpr double kFrameDpi = 600;
constexpr double kFramePxPerMm = kFrameDpi / 25.4;

Element element(const char* type, Box box, double score = 0.9) {
  return Element{type, type, score, box, box};
}

Detection detection(const char* raw, Box box, double score) {
  return Detection{raw, "", "", score, box};
}

const Box kPage{0, 0, 3500, 5000};
const Box kPaper{0, 0, 3500, 5000};
}  // namespace

BOOST_AUTO_TEST_CASE(test_include_list) {
  QString error;
  BOOST_CHECK_EQUAL(parseIncludeList("page-numbers,tabs", &error), kIncludePageNumbers | kIncludeTabs);
  BOOST_CHECK(error.isEmpty());
  BOOST_CHECK_EQUAL(parseIncludeList(" clean-edge , marginalia,headers", &error),
                    kCleanPaperEdge | kIncludeMarginalia | kIncludeHeaders);
  BOOST_CHECK_EQUAL(parseIncludeList("all", &error), kIncludeAll);
  BOOST_CHECK_EQUAL(parseIncludeList("none", &error), 0u);
  BOOST_CHECK(error.isEmpty());
  BOOST_CHECK_EQUAL(parseIncludeList("tabs,footers", &error), 0u);
  BOOST_CHECK(error.contains("footers"));
  BOOST_CHECK((includeListNames(kIncludeAll)
               == QStringList{"page-numbers", "tabs", "headers", "marginalia", "clean-edge"}));
  BOOST_CHECK_EQUAL(includeFlagOfType("page_number"), kIncludePageNumbers);
  BOOST_CHECK_EQUAL(includeFlagOfType("tab"), kIncludeTabs);
  BOOST_CHECK_EQUAL(includeFlagOfType("text"), 0u);
}

BOOST_AUTO_TEST_CASE(test_final_box_union) {
  const Box autoBox{500, 400, 3000, 4500};
  const std::vector<Element> els{element("tab", Box{3200, 1000, 3400, 1600}),
                                 element("page_number", Box{2800, 4700, 2950, 4780}),
                                 element("header", Box{600, 200, 2900, 300})};
  // Nothing on: the automatic box.
  FinalBox fb = finalBox(autoBox, kPage, els, {}, kPaper, 0, PageSide::Right, kFrameDpi);
  BOOST_CHECK((*fb.box == autoBox));
  BOOST_CHECK(fb.included.empty());
  // Page numbers only: the tab and the header stay out.
  fb = finalBox(autoBox, kPage, els, {}, kPaper, kIncludePageNumbers, PageSide::Right, kFrameDpi);
  BOOST_CHECK((*fb.box == Box{500, 400, 3000, 4780}));
  BOOST_CHECK((fb.included == std::vector<int>{1}));
  // Page numbers + tabs + headers.
  fb = finalBox(autoBox, kPage, els, {}, kPaper, kIncludePageNumbers | kIncludeTabs | kIncludeHeaders,
                PageSide::Right, kFrameDpi);
  BOOST_CHECK((*fb.box == Box{500, 200, 3400, 4780}));
  BOOST_CHECK_EQUAL(fb.included.size(), 3u);
  BOOST_CHECK(fb.cleaned.isEmpty());
}

BOOST_AUTO_TEST_CASE(test_final_box_clipped_and_empty) {
  // Elements are clipped to the page rectangle; one fully outside is not included.
  const Box page{100, 100, 3400, 4900};
  const std::vector<Element> els{element("page_number", Box{3300, 4800, 3600, 5000}),
                                 element("marginalia", Box{3450, 2000, 3500, 2100})};
  FinalBox fb = finalBox(Box{500, 400, 3000, 4500}, page, els, {}, kPaper, kIncludeAll & ~kCleanPaperEdge,
                         PageSide::Single, kFrameDpi);
  BOOST_CHECK((*fb.box == Box{500, 400, 3400, 4900}));
  BOOST_CHECK((fb.included == std::vector<int>{0}));
  // No automatic box: the elements alone; nothing at all: no box.
  fb = finalBox(std::nullopt, page, els, {}, kPaper, kIncludePageNumbers, PageSide::Single, kFrameDpi);
  BOOST_CHECK((*fb.box == Box{3300, 4800, 3400, 4900}));
  fb = finalBox(std::nullopt, page, els, {}, kPaper, kIncludeTabs | kCleanPaperEdge, PageSide::Single, kFrameDpi);
  BOOST_CHECK(!fb.box);
}

BOOST_AUTO_TEST_CASE(test_clean_edge_outer_to_tab) {
  // Right page: the automatic box runs over the tab into the next leaves' strips at the edge.
  const Box autoBox{500, 400, 3450, 4500};
  const std::vector<Element> els{element("tab", Box{3200, 1000, 3380, 1600}, 0.8),
                                 element("tab", Box{3250, 3000, 3420, 3600}, 0.6)};  // lower score: ignored
  const std::vector<Detection> dets{detection("text", Box{520, 420, 3000, 4480}, 0.95)};
  const FinalBox fb
      = finalBox(autoBox, kPage, els, dets, kPaper, kIncludeTabs | kCleanPaperEdge, PageSide::Right, kFrameDpi);
  // Outer side at the best tab's outer edge; it is not cut again to the model's extent.
  BOOST_CHECK_EQUAL(fb.box->x1, 3380);
  BOOST_CHECK((fb.cleaned == QStringList{"outer to tab"}));
  // Without the edge toggle: the union only.
  const FinalBox plain = finalBox(autoBox, kPage, els, dets, kPaper, kIncludeTabs, PageSide::Right, kFrameDpi);
  BOOST_CHECK_EQUAL(plain.box->x1, 3450);
  // A left page: the outer side is the left one; the tab is inside the box there, nothing to cut.
  const FinalBox left = finalBox(Box{20, 400, 3000, 4500}, kPage, {element("tab", Box{10, 1000, 200, 1600})},
                                 dets, kPaper, kIncludeTabs | kCleanPaperEdge, PageSide::Left, kFrameDpi);
  BOOST_CHECK_EQUAL(left.box->x0, 10);  // grown by the tab, not cut (the extent includes the tab)
}

BOOST_AUTO_TEST_CASE(test_clean_edge_model_extent) {
  // Single page; the automatic box touches the paper's top edge (a shadow) and its right edge.
  const Box paper{0, 50, 3400, 5000};
  const Box autoBox{500, 60, 3390, 4500};
  const std::vector<Detection> dets{detection("text", Box{520, 300, 3000, 4480}, 0.9),
                                    detection("picture", Box{400, 100, 600, 200}, 0.4),  // below 0.5: not seen
                                    detection("page_number", Box{2800, 4520, 2900, 4560}, 0.7)};
  const FinalBox fb = finalBox(autoBox, kPage, {}, dets, paper, kCleanPaperEdge, PageSide::Single, kFrameDpi);
  const double pad = 1 * kFramePxPerMm;
  BOOST_CHECK_CLOSE(fb.box->y0, 300 - pad, 1e-9);   // top: within 3 mm of the paper edge
  BOOST_CHECK_CLOSE(fb.box->x1, 3000 + pad, 1e-9);  // right: within 3 mm
  BOOST_CHECK_EQUAL(fb.box->x0, 500);               // left: far from the paper edge
  BOOST_CHECK_EQUAL(fb.box->y1, 4500);              // bottom: far from the paper edge
  BOOST_CHECK((fb.cleaned == QStringList{"top to model extent", "right to model extent"}));
  // A side more than 3 mm from the paper edge is kept.
  const Box inner{500, 60 + 3.1 * kFramePxPerMm, 3390 - 3.1 * kFramePxPerMm, 4500};
  const FinalBox fi = finalBox(inner, kPage, {}, dets, paper, kCleanPaperEdge, PageSide::Single, kFrameDpi);
  BOOST_CHECK((*fi.box == inner));
  // A cut that would leave no area: the box before the cuts.
  const std::vector<Detection> tiny{detection("text", Box{3300, 4900, 3310, 4910}, 0.9)};
  const Box full{0, 50, 1000, 4000};  // left and top touch the paper edge, cut to beyond the right side
  const FinalBox fd = finalBox(full, kPage, {}, tiny, paper, kCleanPaperEdge, PageSide::Single, kFrameDpi);
  BOOST_CHECK((*fd.box == full));
  // Only ever shrinks: a side whose model extent lies beyond it stays.
  const std::vector<Detection> wide{detection("text", Box{-100, 0, 3500, 5100}, 0.9)};
  const FinalBox fw = finalBox(autoBox, kPage, {}, wide, paper, kCleanPaperEdge, PageSide::Single, kFrameDpi);
  BOOST_CHECK((*fw.box == autoBox));
  BOOST_CHECK(fw.cleaned.isEmpty());
}

BOOST_AUTO_TEST_CASE(test_params_xml_toggles) {
  QDomDocument doc;
  select_content::Params params(QRectF(10, 20, 300, 400), QSizeF(12, 17), QRectF(0, 0, 500, 600),
                                select_content::Dependencies(), MODE_AUTO, MODE_DISABLED, false);
  // No toggles: no new attributes or elements (projects stay as they were).
  QDomElement el = params.toXml(doc, "params");
  BOOST_CHECK(!el.hasAttribute("includeTabs"));
  BOOST_CHECK(!el.hasAttribute("cleanPaperEdge"));
  BOOST_CHECK(el.namedItem("auto-content-rect").isNull());
  select_content::Params back(el);
  BOOST_CHECK_EQUAL(back.layoutInclude(), 0u);
  BOOST_CHECK(!back.autoContentRect());
  // Toggles and the automatic box round-trip.
  params.setLayoutInclude(kIncludeTabs | kIncludePageNumbers | kCleanPaperEdge);
  params.setAutoContentRect(QRectF(15, 25, 280, 380));
  el = params.toXml(doc, "params");
  BOOST_CHECK(el.attribute("includeTabs") == "1");
  BOOST_CHECK(el.attribute("includePageNumbers") == "1");
  BOOST_CHECK(!el.hasAttribute("includeHeaders"));
  back = select_content::Params(el);
  BOOST_CHECK_EQUAL(back.layoutInclude(), kIncludeTabs | kIncludePageNumbers | kCleanPaperEdge);
  BOOST_REQUIRE(back.autoContentRect());
  BOOST_CHECK((*back.autoContentRect() == QRectF(15, 25, 280, 380)));
}

BOOST_AUTO_TEST_SUITE_END()
}  // namespace Tests
