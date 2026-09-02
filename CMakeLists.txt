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

set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)

set(CLAP_HELPERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLAP_HELPERS_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    clap_helpers
    GIT_REPOSITORY https://github.com/free-audio/clap-helpers.git
    GIT_TAG c35dd4906bd8efbb900cb2b89e680fed463cc8b1
)
FetchContent_MakeAvailable(clap_helpers)

FetchContent_Declare(
    ara_sdk
    GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
    GIT_TAG releases/2.3.0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ARA_API ARA_Library
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(ara_sdk)
juce_set_ara_sdk_path("${ara_sdk_SOURCE_DIR}")

juce_add_binary_data(
    StudioDuoBrandData
    HEADER_NAME StudioDuoBrandData.h
    NAMESPACE studio_brand
    SOURCES
        assets/branding/studio-duo-icon.svg
        assets/branding/studio-duo-icon-512.png
)

set(STUDIO_DUO_MODEL_SOURCES
    src/automation/AutomationRecorder.cpp
    src/automation/AutomationRecorder.h
    src/automation/AutomationScheduler.cpp
    src/automation/AutomationScheduler.h
    src/automation/AutomationTypes.cpp
    src/automation/AutomationTypes.h
    src/audio/AudioAnalysis.cpp
    src/audio/AudioAnalysis.h
    src/mix/RoutingTypes.cpp
    src/mix/RoutingTypes.h
    src/mix/RoutingGraph.cpp
    src/mix/RoutingGraph.h
    src/mix/RoutingGraphCompiler.cpp
    src/mix/RoutingGraphCompiler.h
    src/mix/SoloResolver.cpp
    src/mix/SoloResolver.h
    src/model/ProjectCommands.cpp
    src/model/ProjectCommands.h
    src/model/ProjectModel.cpp
    src/model/ProjectModel.h
    src/project_io/ProjectFile.cpp
    src/project_io/ProjectFile.h
    src/project_io/ProjectMigration.cpp
    src/project_io/ProjectMigration.h
    src/reamp/ReampSnapshotService.cpp
    src/reamp/ReampSnapshotService.h
)

set(STUDIO_DUO_PLUGIN_SOURCES
    src/devices/DeviceRegistry.cpp
    src/devices/DeviceRegistry.h
    src/devices/UtilityDeviceProcessor.cpp
    src/devices/UtilityDeviceProcessor.h
    src/plugin_host/AraDocumentHost.cpp
    src/plugin_host/AraDocumentHost.h
    src/plugin_host/ClapPluginFormat.cpp
    src/plugin_host/ClapPluginFormat.h
    src/plugin_host/ClapPluginInstance.cpp
    src/plugin_host/ClapPluginInstance.h
    src/plugin_host/PluginBridgeClient.cpp
    src/plugin_host/PluginBridgeClient.h
    src/plugin_host/PluginBridgeProtocol.h
    src/plugin_host/PluginCatalog.cpp
    src/plugin_host/PluginCatalog.h
    src/plugin_host/PluginCompatibilityDatabase.cpp
    src/plugin_host/PluginCompatibilityDatabase.h
    src/plugin_host/PluginCompatibilityValidator.cpp
    src/plugin_host/PluginCompatibilityValidator.h
    src/plugin_host/PluginEditorWindow.cpp
    src/plugin_host/PluginEditorWindow.h
    src/plugin_host/PluginFormats.cpp
    src/plugin_host/PluginFormats.h
    src/plugin_host/PluginStateStore.cpp
    src/plugin_host/PluginStateStore.h
    src/plugin_host/ScreamForgeValidation.cpp
    src/plugin_host/ScreamForgeValidation.h
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
        src/render/RenderEngine.cpp
        src/render/RenderEngine.h
        src/ui/MainComponent.cpp
        src/ui/MainComponent.h
        src/ui/AutomationPanel.cpp
        src/ui/AutomationPanel.h
        src/ui/MixerPanel.cpp
        src/ui/MixerPanel.h
        src/ui/PluginInsertPanel.cpp
        src/ui/PluginInsertPanel.h
        src/ui/PluginParameterPanel.cpp
        src/ui/PluginParameterPanel.h
        src/ui/RoutingUiModel.cpp
        src/ui/RoutingUiModel.h
        src/ui/RoutingPanel.cpp
        src/ui/RoutingPanel.h
        src/ui/StudioTheme.h
        src/ui/TimelineComponent.cpp
        src/ui/TimelineComponent.h
        ${STUDIO_DUO_MODEL_SOURCES}
        ${STUDIO_DUO_PLUGIN_SOURCES}
)

if(APPLE)
    target_sources(
        StudioDuo
        PRIVATE
            src/platform/ApplicationIcon.h
            src/platform/MacApplicationIcon.mm
    )
endif()

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
        JUCE_PLUGINHOST_ARA=1
        JUCE_MODAL_LOOPS_PERMITTED=1
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
        juce::juce_cryptography
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_gui_extra
        clap
        clap-helpers
        StudioDuoBrandData
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

    set(STUDIO_DUO_ARA_FIXTURE_FORMATS VST3)
    if(APPLE)
        list(APPEND STUDIO_DUO_ARA_FIXTURE_FORMATS AU)
    endif()
    juce_add_plugin(
        StudioDuoAraFixture
        COMPANY_NAME "Studio Duo"
        COMPANY_WEBSITE "https://github.com/mbianchidev/studio-duo"
        COMPANY_EMAIL "opensource@studio-duo.invalid"
        BUNDLE_ID "dev.mbianchi.studioduo.ara-fixture"
        PLUGIN_MANUFACTURER_CODE StDu
        PLUGIN_CODE ArFt
        FORMATS ${STUDIO_DUO_ARA_FIXTURE_FORMATS}
        PRODUCT_NAME "Studio Duo ARA Fixture"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        IS_ARA_EFFECT TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    juce_generate_juce_header(StudioDuoAraFixture)
    target_sources(
        StudioDuoAraFixture
        PRIVATE
            tests/fixtures/AraTestPlugin.cpp
    )
    target_compile_definitions(
        StudioDuoAraFixture
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
        PUBLIC
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
    target_link_libraries(
        StudioDuoAraFixture
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_plugin_client
    )

    add_library(StudioDuoClapFixture MODULE tests/fixtures/ClapTestPlugin.cpp)
    target_link_libraries(StudioDuoClapFixture PRIVATE clap)
    set_target_properties(
        StudioDuoClapFixture
        PROPERTIES
            PREFIX ""
            OUTPUT_NAME "StudioDuoClapFixture"
    )
    if(APPLE)
        set_target_properties(
            StudioDuoClapFixture
            PROPERTIES
                BUNDLE TRUE
                BUNDLE_EXTENSION clap
                MACOSX_BUNDLE_GUI_IDENTIFIER "dev.mbianchi.studioduo.clap-fixture"
                MACOSX_BUNDLE_BUNDLE_NAME "StudioDuoClapFixture"
                MACOSX_BUNDLE_BUNDLE_VERSION "1"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        )
    elseif(WIN32)
        set_target_properties(StudioDuoClapFixture PROPERTIES SUFFIX ".clap")
    endif()

    juce_add_console_app(StudioDuoTests PRODUCT_NAME "Studio Duo Tests")

    target_sources(
        StudioDuoTests
        PRIVATE
            tests/ModelTests.cpp
            tests/RoutingModelTests.cpp
            tests/RoutingEngineTests.cpp
            tests/RoutingUiModelTests.cpp
            tests/PluginFormatTests.cpp
            tests/PluginRecoveryTests.cpp
            tests/AutomationTests.cpp
            tests/DeviceTests.cpp
            tests/ReampSnapshotTests.cpp
            tests/RenderEngineTests.cpp
            tests/PluginCompatibilityTests.cpp
            tests/ProjectMigrationTests.cpp
            src/ui/RoutingUiModel.cpp
            src/ui/RoutingUiModel.h
            src/audio/StudioAudioEngine.cpp
            src/audio/StudioAudioEngine.h
            src/plugin_host/PluginBridgeClient.cpp
            src/plugin_host/PluginBridgeClient.h
            src/plugin_host/PluginBridgeProtocol.h
            src/plugin_host/AraDocumentHost.cpp
            src/plugin_host/AraDocumentHost.h
            src/plugin_host/ClapPluginFormat.cpp
            src/plugin_host/ClapPluginFormat.h
            src/plugin_host/ClapPluginInstance.cpp
            src/plugin_host/ClapPluginInstance.h
            src/plugin_host/PluginFormats.cpp
            src/plugin_host/PluginFormats.h
            src/plugin_host/PluginCompatibilityDatabase.cpp
            src/plugin_host/PluginCompatibilityDatabase.h
            src/plugin_host/PluginCompatibilityValidator.cpp
            src/plugin_host/PluginCompatibilityValidator.h
            src/plugin_host/PluginEditorWindow.cpp
            src/plugin_host/PluginEditorWindow.h
            src/plugin_host/PluginStateStore.cpp
            src/plugin_host/PluginStateStore.h
            src/plugin_host/ScreamForgeValidation.cpp
            src/plugin_host/ScreamForgeValidation.h
            src/devices/DeviceRegistry.cpp
            src/devices/DeviceRegistry.h
            src/devices/UtilityDeviceProcessor.cpp
            src/devices/UtilityDeviceProcessor.h
            src/render/RenderEngine.cpp
            src/render/RenderEngine.h
            ${STUDIO_DUO_MODEL_SOURCES}
    )

    target_include_directories(StudioDuoTests PRIVATE src)
    target_include_directories(StudioDuoTests SYSTEM PRIVATE ${signalsmith_stretch_SOURCE_DIR})

    target_compile_definitions(
        StudioDuoTests
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_PLUGINHOST_VST=0
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_ARA=1
            JUCE_MODAL_LOOPS_PERMITTED=1
            STUDIO_DUO_TESTING=1
            STUDIO_DUO_VERSION="${PROJECT_VERSION}"
    )
    if(APPLE)
        target_compile_definitions(
            StudioDuoTests
            PRIVATE
                STUDIO_DUO_ARA_FIXTURE_PATH="$<TARGET_BUNDLE_DIR:StudioDuoAraFixture_VST3>"
                STUDIO_DUO_CLAP_FIXTURE_PATH="$<TARGET_BUNDLE_DIR:StudioDuoClapFixture>"
        )
    else()
        target_compile_definitions(
            StudioDuoTests
            PRIVATE
                STUDIO_DUO_ARA_FIXTURE_PATH="$<TARGET_FILE:StudioDuoAraFixture_VST3>"
                STUDIO_DUO_CLAP_FIXTURE_PATH="$<TARGET_FILE:StudioDuoClapFixture>"
        )
    endif()
    add_dependencies(
        StudioDuoTests
        StudioDuoAraFixture_VST3
        StudioDuoClapFixture
    )
    if(APPLE)
        target_compile_definitions(StudioDuoTests PRIVATE JUCE_PLUGINHOST_AU=1)
    else()
        target_compile_definitions(StudioDuoTests PRIVATE JUCE_PLUGINHOST_AU=0)
    endif()

    target_link_libraries(
        StudioDuoTests
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_devices
            juce::juce_audio_processors
            juce::juce_audio_utils
            juce::juce_cryptography
            juce::juce_data_structures
            juce::juce_dsp
            juce::juce_graphics
            clap
            clap-helpers
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    add_test(NAME StudioDuoTests COMMAND StudioDuoTests)
endif()
