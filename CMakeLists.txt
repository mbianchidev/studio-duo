cmake_minimum_required(VERSION 3.25)

project(
    StudioDuo
    VERSION 0.1.0
    DESCRIPTION "A native open-source DAW built for modern metal production"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STUDIO_DUO_BUILD_TESTS "Build Studio Duo tests" ON)
set(STUDIO_DUO_BUNDLE_ID "dev.mbianchi.studioduo")

include(FetchContent)
find_package(JUCE CONFIG QUIET)

if(NOT TARGET juce::juce_gui_extra)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(signalsmith_stretch)

set(STUDIO_DUO_MODEL_SOURCES
    src/audio/AudioAnalysis.cpp
    src/audio/AudioAnalysis.h
    src/model/ProjectCommands.cpp
    src/model/ProjectCommands.h
    src/model/ProjectModel.cpp
    src/model/ProjectModel.h
    src/project_io/ProjectFile.cpp
    src/project_io/ProjectFile.h
)

set(STUDIO_DUO_PLUGIN_SOURCES
    src/plugin_host/PluginBridgeClient.cpp
    src/plugin_host/PluginBridgeClient.h
    src/plugin_host/PluginBridgeProtocol.h
    src/plugin_host/PluginCatalog.cpp
    src/plugin_host/PluginCatalog.h
)

juce_add_gui_app(
    StudioDuo
    PRODUCT_NAME "Studio Duo"
    COMPANY_NAME "Studio Duo"
    BUNDLE_ID "${STUDIO_DUO_BUNDLE_ID}"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/studio-duo-icon-512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/studio-duo-icon-32.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Studio Duo needs microphone access to record audio."
)

juce_generate_juce_header(StudioDuo)

target_sources(
    StudioDuo
    PRIVATE
        src/Main.cpp
        src/audio/StudioAudioEngine.cpp
        src/audio/StudioAudioEngine.h
        src/audio/RecordingWaveform.h
        src/plugin_host/PluginBrowserComponent.cpp
        src/plugin_host/PluginBrowserComponent.h
        src/plugin_host/PluginBridgeWorker.cpp
        src/plugin_host/PluginBridgeWorker.h
        src/plugin_host/PluginScanWorker.cpp
        src/plugin_host/PluginScanWorker.h
        src/ui/MainComponent.cpp
        src/ui/MainComponent.h
        src/ui/StudioTheme.h
        src/ui/TimelineComponent.cpp
        src/ui/TimelineComponent.h
        ${STUDIO_DUO_MODEL_SOURCES}
        ${STUDIO_DUO_PLUGIN_SOURCES}
)

target_include_directories(
    StudioDuo
    PRIVATE
        src
)
target_include_directories(StudioDuo SYSTEM PRIVATE ${signalsmith_stretch_SOURCE_DIR})

target_compile_definitions(
    StudioDuo
    PRIVATE
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_VST3=1
        STUDIO_DUO_VERSION="${PROJECT_VERSION}"
)

if(APPLE)
    target_compile_definitions(StudioDuo PRIVATE JUCE_PLUGINHOST_AU=1)
else()
    target_compile_definitions(StudioDuo PRIVATE JUCE_PLUGINHOST_AU=0)
endif()

target_link_libraries(
    StudioDuo
    PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_data_structures
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(APPLE)
    add_custom_command(
        TARGET StudioDuo
        POST_BUILD
        COMMAND /usr/bin/codesign
            --force
            --deep
            --sign -
            --identifier "${STUDIO_DUO_BUNDLE_ID}"
            --requirements "=designated => identifier \"${STUDIO_DUO_BUNDLE_ID}\""
            "$<TARGET_BUNDLE_DIR:StudioDuo>"
        COMMENT "Applying stable development signature to Studio Duo"
        VERBATIM
    )
endif()

if(STUDIO_DUO_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(StudioDuoTests PRODUCT_NAME "Studio Duo Tests")

    target_sources(
        StudioDuoTests
        PRIVATE
            tests/ModelTests.cpp
            ${STUDIO_DUO_MODEL_SOURCES}
    )

    target_include_directories(StudioDuoTests PRIVATE src)

    target_compile_definitions(
        StudioDuoTests
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            STUDIO_DUO_VERSION="${PROJECT_VERSION}"
    )

    target_link_libraries(
        StudioDuoTests
        PRIVATE
            juce::juce_audio_formats
            juce::juce_data_structures
            juce::juce_graphics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    add_test(NAME StudioDuoTests COMMAND StudioDuoTests)
endif()
