set(CMAKE_AUTOUIC ON)

set(ui_files
    ApplyDialog.ui
    OptionsWidget.ui)

set(sources
    ImageView.cpp ImageView.h
    Filter.cpp Filter.h
    OptionsWidget.cpp OptionsWidget.h
    ApplyDialog.cpp ApplyDialog.h
    ContentBoxFinder.cpp ContentBoxFinder.h
    PageFinder.cpp PageFinder.h
    Task.cpp Task.h
    CacheDrivenTask.cpp CacheDrivenTask.h
    Dependencies.cpp Dependencies.h
    Params.cpp Params.h
    Settings.cpp Settings.h
    Thumbnail.cpp Thumbnail.h
    PhysSizeCalc.cpp PhysSizeCalc.h
    OrderByWidthProvider.cpp OrderByWidthProvider.h
    OrderByHeightProvider.cpp OrderByHeightProvider.h
    Utils.cpp Utils.h
    LayoutImage.cpp LayoutImage.h
    LayoutRules.cpp LayoutRules.h
    LayoutDetector.cpp LayoutDetector.h)

add_library(select_content STATIC ${sources} ${ui_files})
target_link_libraries(select_content PUBLIC core)
if (HAVE_LAYOUT_DETECTOR)
  target_compile_definitions(select_content PRIVATE HAVE_LAYOUT_DETECTOR)
  target_link_libraries(select_content PRIVATE ${LAYOUT_DETECTOR_LIBS})
endif()

translation_sources(scantailor ${sources} ${ui_files})
