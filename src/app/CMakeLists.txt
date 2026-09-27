# The Linux front end: Qt 6 Widgets over the portable core.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Svg)
# DBus lets "System" appearance ask the desktop portal for the colour scheme (needed inside an AppImage).
# Not on Windows, which has no session bus.
if(NOT WIN32)
    find_package(Qt6 6.4 QUIET COMPONENTS DBus)
endif()
option(COMPOSITOR_WITH_QTPDF "Open PDF files through Qt PDF when it is installed" ON)
if(COMPOSITOR_WITH_QTPDF)
    find_package(Qt6 6.4 QUIET OPTIONAL_COMPONENTS Pdf)
endif()

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

qt_add_executable(nekophoto
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
    EditorSessionStyles.cpp
    EditorSessionQuickMask.cpp
    EditorSessionPaths.cpp
    LayerStyleDialog.cpp
    PathsPanel.h
    PathsPanel.cpp
    EditorSessionPainting.cpp
    EditorSessionPixels.cpp
    EditorSessionQuickSelect.cpp
    EditorSessionSelection.cpp
    EditorSessionSmartObjects.cpp
    EditorSessionArtboards.cpp
    CanvasWidgetBoxes.cpp
    EditorSessionSmartFilters.cpp
    EditorSessionWatch.cpp
    EditorSessionTransform.cpp
    CanvasWidget.h
    CanvasWidget.cpp
    CanvasWidgetInput.cpp
    CanvasWidgetOverlays.cpp
    CanvasWidgetPointer.cpp
    LayersPanel.h
    LayersPanel.cpp
    AdjustmentEditor.h
    AdjustmentEditor.cpp
    AdjustmentsPanel.h
    AdjustmentsPanel.cpp
    BrushLibrary.h
    BrushLibrary.cpp
    BrushImporter.h
    BrushImporter.cpp
    PresetLibrary.h
    PresetLibrary.cpp
    ActionLibrary.h
    ActionLibrary.cpp
    ActionsPanel.h
    ActionsPanel.cpp
    TimelinePanel.h
    TimelinePanel.cpp
    AutomationActions.cpp
    EditorSessionTimeline.cpp
    BrushPicker.h
    BrushPicker.cpp
    FilterDialog.h
    FilterDialog.cpp
    SmartFilterDialog.h
    SmartFilterDialog.cpp
    CameraRawDialog.h
    CameraRawDialog.cpp
    PixelDialog.h
    PixelDialog.cpp
    ContentFillDialog.cpp
    ContentAwareScaleDialog.h
    ContentAwareScaleDialog.cpp
    Gmic.h
    Gmic.cpp
    GmicDialog.h
    GmicDialog.cpp
    TextLayer.h
    TextLayer.cpp
    TextDialog.h
    TextDialog.cpp
    RichText.h
    RichText.cpp
    WarpDialog.h
    WarpDialog.cpp
    FontPicker.h
    FontPicker.cpp
    SingleInstance.cpp
    Platform.h
    Platform.cpp
    FontGroups.h
    FontGroups.cpp
    Automation.h
    Automation.cpp
    AutomationDescriptions.cpp
    AutomationDocument.cpp
    AutomationArtboards.cpp
    AutomationHandlers.h
    AutomationLayers.cpp
    AutomationPaint.cpp
    AutomationPixels.cpp
    AutomationSelection.cpp
    AutomationSupport.cpp
    Theme.h
    Theme.cpp
    ModelStore.h
    ModelStore.cpp
    PreferencesDialog.h
    PreferencesDialog.cpp
    ToolOptionsBar.h
    ToolOptionsBar.cpp
    WelcomeDialog.h
    WelcomeDialog.cpp
    Dialogs.h
    Dialogs.cpp
    MainWindow.h
    MainWindow.cpp
    MainWindowFiles.cpp
    MainWindowMenus.cpp
    Autosave.h
    Autosave.cpp
    Bench.h
    Bench.cpp
    VectorFiles.h
    VectorFiles.cpp
    Language.h
    Language.cpp
    Names.h
    Names.cpp
)

target_compile_definitions(nekophoto PRIVATE COMPOSITOR_VERSION="${COMPOSITOR_VERSION_STRING}")
# The MyPaint brush presets (CC0) are compiled in, so every package and a plain build tree have them.
file(GLOB_RECURSE COMPOSITOR_BRUSH_FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/brushes/mypaint/*.myb ${CMAKE_CURRENT_SOURCE_DIR}/brushes/mypaint/*_prev.png)
# G'MIC filters that do not work here at their defaults (tools/gmic-sweep.py makes the list).
qt_add_resources(nekophoto "welcome" PREFIX "/" FILES images/welcome.jpg)
qt_add_resources(nekophoto "gmic_lists" PREFIX "/" FILES gmic/unsupported.txt)
qt_add_resources(nekophoto "mypaint_brushes" PREFIX "/" FILES ${COMPOSITOR_BRUSH_FILES} brushes/mypaint/order.conf)
# Translations (Qt Linguist): translations/nekophoto_<lang>.ts is compiled to .qm and embedded under :/i18n, so
# every package carries it. Optional: without Qt LinguistTools the app builds, in English only.
# `cmake --build build --target update_translations` refreshes the .ts from the sources (docs/translating.md).
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease AND TARGET Qt6::lupdate)
    qt_add_translations(nekophoto TS_FILES ${CMAKE_SOURCE_DIR}/translations/nekophoto_ja.ts RESOURCE_PREFIX "/i18n"
        LUPDATE_OPTIONS -no-obsolete -locations none)
    message(STATUS "Translations: Japanese (Qt LinguistTools ${Qt6LinguistTools_VERSION})")
else()
    message(STATUS "Translations: Qt LinguistTools not found (qt6-l10n-tools on Debian/Ubuntu, qt6-tools elsewhere); the app will be English only")
endif()
target_link_libraries(nekophoto PRIVATE compositor::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg compositor_warnings)
# Qt PDF (PDFium, BSD-3 / LGPL Qt) renders PDF pages for File > Open. Optional: without it PDFs are refused with a
# message. Found at the top as an optional component of the one Qt lookup (a separate find_package(Qt6Pdf) after the
# target exists loaded Qt's packages a second time on Homebrew and left the app without its moc output).
if(COMPOSITOR_WITH_QTPDF AND TARGET Qt6::Pdf)
    target_link_libraries(nekophoto PRIVATE Qt6::Pdf)
    target_compile_definitions(nekophoto PRIVATE COMPOSITOR_HAVE_QTPDF=1)
    message(STATUS "PDF: Qt PDF ${Qt6Pdf_VERSION}")
else()
    message(STATUS "PDF: Qt PDF not found (or off), PDF import disabled")
endif()
# Off by default: linking libgmic makes it a hard dependency of the binary, and an installed build then
# refuses to start once the library is upgraded or removed, while the gmic executable serves the same
# filters. In-process G'MIC is for people who build with it on and keep libgmic around.
option(COMPOSITOR_WITH_LIBGMIC "Run G'MIC filters in-process through libgmic when it is installed" OFF)
if(COMPOSITOR_WITH_LIBGMIC)
    find_path(GMIC_INCLUDE_DIR gmic.h)
    find_library(GMIC_LIBRARY gmic)
    if(GMIC_INCLUDE_DIR AND GMIC_LIBRARY)
        target_include_directories(nekophoto PRIVATE ${GMIC_INCLUDE_DIR})
        target_link_libraries(nekophoto PRIVATE ${GMIC_LIBRARY})
        target_compile_definitions(nekophoto PRIVATE COMPOSITOR_HAVE_LIBGMIC)
        message(STATUS "G'MIC: in-process through libgmic (${GMIC_LIBRARY})")
    else()
        message(STATUS "G'MIC: libgmic not found; filters run through the gmic executable when it is installed")
    endif()
endif()
if(TARGET Qt6::DBus)
    target_link_libraries(nekophoto PRIVATE Qt6::DBus)
    target_compile_definitions(nekophoto PRIVATE COMPOSITOR_HAVE_DBUS)
    message(STATUS "Appearance: desktop colour scheme via the XDG portal (Qt DBus)")
endif()
if(WIN32)
    # A GUI-subsystem program (no console window when started from Explorer) that attaches to the calling
    # console for --version, --help and --call (Platform.cpp). The resource file carries the icon, the version
    # information and the manifest (UTF-8 code page for the core's narrow file APIs).
    set_target_properties(nekophoto PROPERTIES WIN32_EXECUTABLE TRUE)
    set(COMPOSITOR_WIN_ICON "${CMAKE_SOURCE_DIR}/packaging/windows/nekophoto.ico")
    set(COMPOSITOR_WIN_MANIFEST "${CMAKE_SOURCE_DIR}/packaging/windows/nekophoto.manifest")
    # FILEVERSION wants four numbers; a pre-release suffix ("1.6.0-dev.abc") is dropped there and kept in the strings.
    string(REGEX MATCHALL "[0-9]+" COMPOSITOR_WIN_VERSION_PARTS "${COMPOSITOR_VERSION_STRING}")
    list(APPEND COMPOSITOR_WIN_VERSION_PARTS 0 0 0 0)
    list(SUBLIST COMPOSITOR_WIN_VERSION_PARTS 0 4 COMPOSITOR_WIN_VERSION_PARTS)
    string(REPLACE ";" "," COMPOSITOR_WIN_VERSION_COMMAS "${COMPOSITOR_WIN_VERSION_PARTS}")
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/nekophoto.rc.in ${CMAKE_CURRENT_BINARY_DIR}/nekophoto.rc @ONLY)
    target_sources(nekophoto PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nekophoto.rc)
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/nekophoto.rc APPEND PROPERTY OBJECT_DEPENDS ${COMPOSITOR_WIN_ICON} ${COMPOSITOR_WIN_MANIFEST})
endif()

# nekophoto on Linux (the command people type); NekoPhoto.app on the Mac, as apps there are named.
if(APPLE)
    set_target_properties(nekophoto PROPERTIES OUTPUT_NAME NekoPhoto)
else()
    set_target_properties(nekophoto PROPERTIES OUTPUT_NAME nekophoto)
endif()

# GPL-3.0 for the program, the MIT notice of the code it ports, and the third-party notices.
set(COMPOSITOR_LICENSE_FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md)
file(GLOB COMPOSITOR_LICENSE_TEXTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/LICENSES/*.txt)

if(APPLE)
    # An app bundle with the icon and document types; macdeployqt then adds Qt and the other dylibs.
    set(COMPOSITOR_ICNS ${CMAKE_SOURCE_DIR}/packaging/nekophoto.icns)
    target_sources(nekophoto PRIVATE ${COMPOSITOR_ICNS})
    set_source_files_properties(${COMPOSITOR_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The licence texts travel inside the bundle, as they do in share/doc on Linux.
    target_sources(nekophoto PRIVATE ${COMPOSITOR_LICENSE_FILES} ${COMPOSITOR_LICENSE_TEXTS})
    set_source_files_properties(${COMPOSITOR_LICENSE_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
    set_source_files_properties(${COMPOSITOR_LICENSE_TEXTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses/LICENSES)
    set_target_properties(nekophoto PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "NekoPhoto"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.vomitselfie.nekophoto"
        MACOSX_BUNDLE_ICON_FILE "nekophoto.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${COMPOSITOR_VERSION_STRING}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${COMPOSITOR_VERSION_STRING}")
    install(TARGETS nekophoto BUNDLE DESTINATION .)
elseif(WIN32)
    # A portable folder: nekophoto.exe with its DLLs beside it (tools/package-windows.sh) and the licences.
    install(TARGETS nekophoto RUNTIME DESTINATION .)
    install(FILES ${COMPOSITOR_LICENSE_FILES} DESTINATION .)
    install(FILES ${COMPOSITOR_LICENSE_TEXTS} DESTINATION LICENSES)
else()
    install(TARGETS nekophoto RUNTIME DESTINATION bin)
endif()
if(NOT WIN32)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/nekophoto.desktop DESTINATION share/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/nekophoto.svg DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/nekophoto-mime.xml DESTINATION share/mime/packages RENAME nekophoto.xml)
endif()
if(NOT APPLE AND NOT WIN32)
    install(FILES ${COMPOSITOR_LICENSE_FILES} DESTINATION share/doc/nekophoto)
    install(FILES ${COMPOSITOR_LICENSE_TEXTS} DESTINATION share/doc/nekophoto/LICENSES)
endif()
