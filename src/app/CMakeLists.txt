# The Linux front end: Qt 6 Widgets over the portable core.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

# Some locked-down shells cannot execute binaries under /usr/lib. Point this at a
# directory holding copies of moc, uic and rcc to build there anyway.
set(COMPOSITOR_QT_TOOL_DIR "" CACHE PATH "Directory with moc/uic/rcc to use instead of Qt's own")
if(COMPOSITOR_QT_TOOL_DIR)
    foreach(tool moc uic rcc)
        if(TARGET Qt6::${tool} AND EXISTS "${COMPOSITOR_QT_TOOL_DIR}/${tool}")
            set_target_properties(Qt6::${tool} PROPERTIES
                IMPORTED_LOCATION "${COMPOSITOR_QT_TOOL_DIR}/${tool}"
                IMPORTED_LOCATION_RELEASE "${COMPOSITOR_QT_TOOL_DIR}/${tool}"
                IMPORTED_LOCATION_DEBUG "${COMPOSITOR_QT_TOOL_DIR}/${tool}"
                IMPORTED_LOCATION_RELWITHDEBINFO "${COMPOSITOR_QT_TOOL_DIR}/${tool}"
                IMPORTED_LOCATION_MINSIZEREL "${COMPOSITOR_QT_TOOL_DIR}/${tool}")
            string(TOUPPER ${tool} upper)
            set(QT_${upper}_EXECUTABLE "${COMPOSITOR_QT_TOOL_DIR}/${tool}" CACHE FILEPATH "" FORCE)
        endif()
    endforeach()
endif()

qt_standard_project_setup()

qt_add_executable(compositor
    main.cpp
    ImageConvert.h
    Style.h
    Viewport.h
    EditorSession.h
    EditorSession.cpp
    CanvasWidget.h
    CanvasWidget.cpp
    LayersPanel.h
    LayersPanel.cpp
    AdjustmentEditor.h
    AdjustmentEditor.cpp
    AdjustmentsPanel.h
    AdjustmentsPanel.cpp
    FilterDialog.h
    FilterDialog.cpp
    ModelStore.h
    ModelStore.cpp
    PreferencesDialog.h
    PreferencesDialog.cpp
    ToolOptionsBar.h
    ToolOptionsBar.cpp
    Dialogs.h
    Dialogs.cpp
    MainWindow.h
    MainWindow.cpp
)

target_link_libraries(compositor PRIVATE compositor::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network compositor_warnings)
set_target_properties(compositor PROPERTIES OUTPUT_NAME compositor)

install(TARGETS compositor RUNTIME DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor.desktop DESTINATION share/applications)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-mime.xml DESTINATION share/mime/packages RENAME compositor.xml)
