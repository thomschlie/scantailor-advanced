set(sources
    main.cpp
    tst_batch_qt.cpp
    tst_core_qt.cpp
    tst_ux_qt.cpp)

add_executable(qt_tests ${sources})
target_link_libraries(
    qt_tests
    PRIVATE core foundation ${Qt_Test_lib} ${Qt_Core_lib} ${Qt_Gui_lib} ${Qt_Widgets_lib} ${EXTRA_LIBS})
target_include_directories(qt_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
# select_content's OptionsWidget (layout toggles test) includes its generated ui_OptionsWidget.h.
target_link_libraries(qt_tests PRIVATE select_content)
target_include_directories(qt_tests PRIVATE "$<TARGET_PROPERTY:select_content,BINARY_DIR>/select_content_autogen/include")

add_test(NAME qt_tests COMMAND qt_tests)
# The tests need no window system; without this, ctest on a machine without a display
# (e.g. CI) aborts at startup ("could not connect to display").
set_tests_properties(qt_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
