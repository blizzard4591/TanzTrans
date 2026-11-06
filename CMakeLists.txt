cmake_minimum_required(VERSION 3.25)
project(TanzTrans VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql LinguistTools Test)
qt_standard_project_setup()

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

# --- core library (no widgets) -------------------------------------------------
add_library(tanztrans_core STATIC
    src/core/Result.h
    src/core/Log.h src/core/Log.cpp
    src/core/ShareLink.h src/core/ShareLink.cpp
    src/core/Settings.h src/core/Settings.cpp
    src/core/WinUtil.h src/core/WinUtil.cpp
    src/core/Probe.h src/core/Probe.cpp
    src/core/Policy.h src/core/Policy.cpp
    src/core/FfmpegArgs.h src/core/FfmpegArgs.cpp
    src/core/FileRecord.h src/core/FileRecord.cpp
    src/core/Database.h src/core/Database.cpp
    src/core/HiDriveClient.h src/core/HiDriveClient.cpp
    src/core/Encoder.h src/core/Encoder.cpp
    src/core/Tools.h src/core/Tools.cpp
    src/core/FfmpegInstaller.h src/core/FfmpegInstaller.cpp
    src/core/JobManager.h src/core/JobManager.cpp
)
target_include_directories(tanztrans_core PUBLIC src/core)
target_link_libraries(tanztrans_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql)
target_compile_definitions(tanztrans_core PUBLIC TT_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(tanztrans_core PUBLIC crypt32)
endif()

# --- application ------------------------------------------------------------------
qt_add_executable(TanzTrans WIN32
    src/app/main.cpp
    src/app/AppIcon.h src/app/AppIcon.cpp
    src/app/Translation.h src/app/Translation.cpp
    src/app/QueueModel.h src/app/QueueModel.cpp
    src/app/LogView.h src/app/LogView.cpp
    src/app/SettingsPage.h src/app/SettingsPage.cpp
    src/app/ToolsDialog.h src/app/ToolsDialog.cpp
    src/app/MainWindow.h src/app/MainWindow.cpp
)
target_link_libraries(TanzTrans PRIVATE tanztrans_core Qt6::Widgets)

qt_add_translations(TanzTrans
    SOURCE_TARGETS TanzTrans tanztrans_core
    TS_FILES translations/tanztrans_de.ts
    LUPDATE_OPTIONS -no-obsolete -locations none
)

# --- tests --------------------------------------------------------------------------
enable_testing()
qt_add_executable(tst_core tests/tst_core.cpp)
target_link_libraries(tst_core PRIVATE tanztrans_core Qt6::Test)
set_target_properties(tst_core PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(tst_core PRIVATE TT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME tst_core COMMAND tst_core)
