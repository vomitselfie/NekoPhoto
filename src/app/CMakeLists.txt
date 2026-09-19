# The Linux front end: Qt 6 Widgets over the portable core.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Svg)
# DBus lets "System" appearance ask the desktop portal for the colour scheme (needed inside an AppImage).
find_package(Qt6 6.4 QUIET COMPONENTS DBus)

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
set(CMAKE_AUTORCC ON)

qt_add_executable(compositor-linux
    main.cpp
    ImageConvert.h
    Style.h
    Icons.h
    ColorSwatches.h
    Ruler.h
    CanvasFrame.h
    icons.qrc
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
    Automation.h
    Automation.cpp
    Theme.h
    Theme.cpp
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

target_compile_definitions(compositor-linux PRIVATE COMPOSITOR_VERSION="${COMPOSITOR_VERSION}")
target_link_libraries(compositor-linux PRIVATE compositor::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg compositor_warnings)
if(TARGET Qt6::DBus)
    target_link_libraries(compositor-linux PRIVATE Qt6::DBus)
    target_compile_definitions(compositor-linux PRIVATE COMPOSITOR_HAVE_DBUS)
    message(STATUS "Appearance: desktop colour scheme via the XDG portal (Qt DBus)")
endif()
set_target_properties(compositor-linux PROPERTIES OUTPUT_NAME compositor-linux)

install(TARGETS compositor-linux RUNTIME DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux.desktop DESTINATION share/applications)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux-mime.xml DESTINATION share/mime/packages RENAME compositor-linux.xml)
