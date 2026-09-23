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
    QtGeometry.h
    Style.h
    Icons.h
    ColorSwatches.h
    Ruler.h
    CanvasFrame.h
    icons.qrc
    Viewport.h
    EditorSession.h
    EditorSession.cpp
    EditorSessionAdjustments.cpp
    EditorSessionLayers.cpp
    EditorSessionPainting.cpp
    EditorSessionPixels.cpp
    EditorSessionQuickSelect.cpp
    EditorSessionSelection.cpp
    EditorSessionTransform.cpp
    CanvasWidget.h
    CanvasWidget.cpp
    LayersPanel.h
    LayersPanel.cpp
    AdjustmentEditor.h
    AdjustmentEditor.cpp
    AdjustmentsPanel.h
    AdjustmentsPanel.cpp
    BrushLibrary.h
    BrushLibrary.cpp
    BrushPicker.h
    BrushPicker.cpp
    FilterDialog.h
    FilterDialog.cpp
    PixelDialog.h
    PixelDialog.cpp
    Gmic.h
    Gmic.cpp
    GmicDialog.h
    GmicDialog.cpp
    TextLayer.h
    TextLayer.cpp
    TextDialog.h
    TextDialog.cpp
    FontPicker.h
    FontPicker.cpp
    SingleInstance.cpp
    FontGroups.h
    FontGroups.cpp
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

target_compile_definitions(compositor-linux PRIVATE COMPOSITOR_VERSION="${COMPOSITOR_VERSION_STRING}")
# The MyPaint brush presets (CC0) are compiled in, so every package and a plain build tree have them.
file(GLOB_RECURSE COMPOSITOR_BRUSH_FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/brushes/mypaint/*.myb ${CMAKE_CURRENT_SOURCE_DIR}/brushes/mypaint/*_prev.png)
qt_add_resources(compositor-linux "mypaint_brushes" PREFIX "/" FILES ${COMPOSITOR_BRUSH_FILES} brushes/mypaint/order.conf)
target_link_libraries(compositor-linux PRIVATE compositor::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg compositor_warnings)
# Off by default: linking libgmic makes it a hard dependency of the binary, and an installed build then
# refuses to start once the library is upgraded or removed, while the gmic executable serves the same
# filters. In-process G'MIC is for people who build with it on and keep libgmic around.
option(COMPOSITOR_WITH_LIBGMIC "Run G'MIC filters in-process through libgmic when it is installed" OFF)
if(COMPOSITOR_WITH_LIBGMIC)
    find_path(GMIC_INCLUDE_DIR gmic.h)
    find_library(GMIC_LIBRARY gmic)
    if(GMIC_INCLUDE_DIR AND GMIC_LIBRARY)
        target_include_directories(compositor-linux PRIVATE ${GMIC_INCLUDE_DIR})
        target_link_libraries(compositor-linux PRIVATE ${GMIC_LIBRARY})
        target_compile_definitions(compositor-linux PRIVATE COMPOSITOR_HAVE_LIBGMIC)
        message(STATUS "G'MIC: in-process through libgmic (${GMIC_LIBRARY})")
    else()
        message(STATUS "G'MIC: libgmic not found; filters run through the gmic executable when it is installed")
    endif()
endif()
if(TARGET Qt6::DBus)
    target_link_libraries(compositor-linux PRIVATE Qt6::DBus)
    target_compile_definitions(compositor-linux PRIVATE COMPOSITOR_HAVE_DBUS)
    message(STATUS "Appearance: desktop colour scheme via the XDG portal (Qt DBus)")
endif()
set_target_properties(compositor-linux PROPERTIES OUTPUT_NAME compositor-linux)

# GPL-3.0 for the program, the MIT notice of the code it ports, and the third-party notices.
set(COMPOSITOR_LICENSE_FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md)
file(GLOB COMPOSITOR_LICENSE_TEXTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/LICENSES/*.txt)

if(APPLE)
    # An app bundle with the icon and document types; macdeployqt then adds Qt and the other dylibs.
    set(COMPOSITOR_ICNS ${CMAKE_SOURCE_DIR}/packaging/compositor-linux.icns)
    target_sources(compositor-linux PRIVATE ${COMPOSITOR_ICNS})
    set_source_files_properties(${COMPOSITOR_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The licence texts travel inside the bundle, as they do in share/doc on Linux.
    target_sources(compositor-linux PRIVATE ${COMPOSITOR_LICENSE_FILES} ${COMPOSITOR_LICENSE_TEXTS})
    set_source_files_properties(${COMPOSITOR_LICENSE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
    set_source_files_properties(${COMPOSITOR_LICENSE_TEXTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses/LICENSES)
    set_target_properties(compositor-linux PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "compositor-linux"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.vomitselfie.compositor-linux"
        MACOSX_BUNDLE_ICON_FILE "compositor-linux.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${COMPOSITOR_VERSION_STRING}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${COMPOSITOR_VERSION_STRING}")
    install(TARGETS compositor-linux BUNDLE DESTINATION .)
else()
    install(TARGETS compositor-linux RUNTIME DESTINATION bin)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux.desktop DESTINATION share/applications)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/compositor-linux-mime.xml DESTINATION share/mime/packages RENAME compositor-linux.xml)
if(NOT APPLE)
    install(FILES ${COMPOSITOR_LICENSE_FILES} DESTINATION share/doc/compositor-linux)
    install(FILES ${COMPOSITOR_LICENSE_TEXTS} DESTINATION share/doc/compositor-linux/LICENSES)
endif()
