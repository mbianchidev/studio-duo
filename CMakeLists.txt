cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STUDIO_DUO_PROJECT_VERSION LIMIT_COUNT 1)
string(STRIP "${STUDIO_DUO_PROJECT_VERSION}" STUDIO_DUO_PROJECT_VERSION)
if(NOT STUDIO_DUO_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 1.2.3")
endif()

project(
    StudioDuo
    VERSION "${STUDIO_DUO_PROJECT_VERSION}"
    DESCRIPTION "A native open-source DAW built for modern metal production"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STUDIO_DUO_BUILD_TESTS "Build Studio Duo tests" ON)
option(STUDIO_DUO_AUDIO_EXPORT_TESTS_ONLY "Build only the headless audio export regression tests" OFF)
set(STUDIO_DUO_BUNDLE_ID "dev.mbianchi.studioduo")

include(FetchContent)

# Official LAME 3.100 source (LGPL-2.0-or-later); no executable or decoder is built.
# The archive hash is also published by Buildroot's package/lame/lame.hash.
FetchContent_Declare(
    studio_duo_lame
    URL https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
    URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(studio_duo_lame)

# A scalar, encoder-only build avoids platform assembly and mpglib entirely.
# Do not use configMS.h: it enables x86 intrinsics even for universal builds.
set(STUDIO_DUO_LAME_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/studio-duo-lame")
file(MAKE_DIRECTORY "${STUDIO_DUO_LAME_CONFIG_DIR}")
file(CONFIGURE OUTPUT "${STUDIO_DUO_LAME_CONFIG_DIR}/config.h" CONTENT [=[
#ifndef STUDIO_DUO_LAME_CONFIG_H
#define STUDIO_DUO_LAME_CONFIG_H
#include <stdint.h>
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define LAME_LIBRARY_BUILD 1
#define NOANALYSIS 1
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;
#endif
]=] @ONLY)
set(STUDIO_DUO_LAME_SOURCES
    VbrTag.c bitstream.c encoder.c fft.c gain_analysis.c id3tag.c
    lame.c newmdct.c presets.c psymodel.c quantize.c quantize_pvt.c
    reservoir.c set_get.c tables.c takehiro.c util.c vbrquantize.c version.c
)
list(TRANSFORM STUDIO_DUO_LAME_SOURCES PREPEND "${studio_duo_lame_SOURCE_DIR}/libmp3lame/")
add_library(StudioDuoMp3Encoder STATIC ${STUDIO_DUO_LAME_SOURCES})
target_include_directories(
    StudioDuoMp3Encoder
    PUBLIC "${studio_duo_lame_SOURCE_DIR}/include"
    PRIVATE "${STUDIO_DUO_LAME_CONFIG_DIR}" "${studio_duo_lame_SOURCE_DIR}/libmp3lame"
)
target_compile_definitions(StudioDuoMp3Encoder PRIVATE HAVE_CONFIG_H=1)
set_target_properties(
    StudioDuoMp3Encoder
    PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED TRUE POSITION_INDEPENDENT_CODE TRUE
)
if(MSVC)
    target_compile_definitions(StudioDuoMp3Encoder PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    target_link_libraries(StudioDuoMp3Encoder PRIVATE m)
endif()

find_package(JUCE CONFIG QUIET)

if(NOT TARGET juce::juce_gui_extra)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

if(STUDIO_DUO_BUILD_TESTS OR STUDIO_DUO_AUDIO_EXPORT_TESTS_ONLY)
    enable_testing()
    juce_add_console_app(StudioDuoAudioExportTests PRODUCT_NAME "Studio Duo Audio Export Tests")
    target_sources(
        StudioDuoAudioExportTests
        PRIVATE tests/AudioExportTests.cpp src/render/AudioExport.cpp src/render/AudioExport.h
    )
    target_include_directories(StudioDuoAudioExportTests PRIVATE src)
    target_compile_definitions(
        StudioDuoAudioExportTests
        PRIVATE
            STUDIO_DUO_AUDIO_EXPORT_TEST_MAIN=1
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_USE_MP3AUDIOFORMAT=1
    )
    target_link_libraries(
        StudioDuoAudioExportTests
        PRIVATE
            juce::juce_audio_formats
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
            StudioDuoMp3Encoder
    )
    add_test(NAME StudioDuoAudioExportTests COMMAND StudioDuoAudioExportTests)
    set_tests_properties(StudioDuoAudioExportTests PROPERTIES TIMEOUT 30)
endif()

if(STUDIO_DUO_AUDIO_EXPORT_TESTS_ONLY)
    return()
endif()

FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(signalsmith_stretch)

set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
unset(LIBSAMPLERATE_TESTS CACHE)
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency test targets" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG 0.2.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libsamplerate)

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

juce_add_binary_data(
    StudioDuoDawProjectSchemaData
    HEADER_NAME StudioDuoDawProjectSchemaData.h
    NAMESPACE studio_dawproject_schema
    SOURCES
        third_party/dawproject/v1.0.0/Project.xsd
        third_party/dawproject/v1.0.0/MetaData.xsd
        third_party/dawproject/v1.0.0/LICENSE
)

set(STUDIO_DUO_MODEL_SOURCES
    src/dawproject_io/DawProjectIdMapper.cpp
    src/dawproject_io/DawProjectIdMapper.h
    src/dawproject_io/DawProjectIO.cpp
    src/dawproject_io/DawProjectIO.h
    src/dawproject_io/DawProjectSchemaValidator.cpp
    src/dawproject_io/DawProjectSchemaValidator.h
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
    src/midi/MidiCaptureBuffer.cpp
    src/midi/MidiCaptureBuffer.h
    src/midi/MidiEditing.cpp
    src/midi/MidiEditing.h
    src/midi/MidiModel.cpp
    src/midi/MidiModel.h
    src/mastering/MasteringEngine.cpp
    src/mastering/MasteringEngine.h
    src/mastering/MasteringModel.cpp
    src/mastering/MasteringModel.h
    src/mastering/MasteringReleaseService.cpp
    src/mastering/MasteringReleaseService.h
    src/model/LinkedEditModel.cpp
    src/model/LinkedEditModel.h
    src/model/ProjectCommands.cpp
    src/model/ProjectCommands.h
    src/model/TransportEditing.cpp
    src/model/TransportEditing.h
    src/model/ProjectModel.cpp
    src/model/ProjectModel.h
    src/project_io/ProjectFile.cpp
    src/project_io/ProjectFile.h
    src/project_io/ProjectCollectionService.cpp
    src/project_io/ProjectCollectionService.h
    src/project_io/ProjectMigration.cpp
    src/project_io/ProjectMigration.h
    src/reamp/ReampSnapshotService.cpp
    src/reamp/ReampSnapshotService.h
    src/render/AudioExport.cpp
    src/render/AudioExport.h
)

set(STUDIO_DUO_PLUGIN_SOURCES
    src/devices/AmpDeviceProcessor.cpp
    src/devices/AmpDeviceProcessor.h
    src/devices/DeviceRegistry.cpp
    src/devices/DeviceRegistry.h
    src/devices/DrumDeviceProcessor.cpp
    src/devices/DrumDeviceProcessor.h
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
    src/plugin_host/PluginSearchPaths.cpp
    src/plugin_host/PluginSearchPaths.h
    src/plugin_host/SampleAccurateAutomationTarget.h
    src/plugin_host/PluginStateStore.cpp
    src/plugin_host/PluginStateStore.h
    src/plugin_host/ValidatedPluginStateTarget.h
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

configure_file(
    "${studio_duo_lame_SOURCE_DIR}/COPYING"
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-COPYING.txt"
    COPYONLY
)
configure_file(
    "${STUDIO_DUO_LAME_CONFIG_DIR}/config.h"
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-config.h"
    COPYONLY
)
get_filename_component(STUDIO_DUO_LAME_SOURCE_PARENT "${studio_duo_lame_SOURCE_DIR}" DIRECTORY)
get_filename_component(STUDIO_DUO_LAME_SOURCE_NAME "${studio_duo_lame_SOURCE_DIR}" NAME)
set(STUDIO_DUO_SOURCE_ARCHIVES
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-3.100-source.tar.gz"
    "${STUDIO_DUO_LAME_CONFIG_DIR}/studio-duo-source.tar.gz"
)
add_custom_target(
    StudioDuoSourceMaterials
    COMMAND "${CMAKE_COMMAND}" -E chdir "${STUDIO_DUO_LAME_SOURCE_PARENT}"
        "${CMAKE_COMMAND}" -E tar czf
        "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-3.100-source.tar.gz"
        "${STUDIO_DUO_LAME_SOURCE_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_COMMAND}" -E tar czf
        "${STUDIO_DUO_LAME_CONFIG_DIR}/studio-duo-source.tar.gz"
        CMakeLists.txt VERSION LICENSE README.md
        src assets third_party docs packaging scripts tests
    BYPRODUCTS ${STUDIO_DUO_SOURCE_ARCHIVES}
    COMMENT "Packaging MP3 library and application source for rebuilding and relinking"
    VERBATIM
)
add_dependencies(StudioDuo StudioDuoSourceMaterials)
set_source_files_properties(${STUDIO_DUO_SOURCE_ARCHIVES} PROPERTIES GENERATED TRUE)
file(CONFIGURE OUTPUT "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-NOTICE.txt" CONTENT [=[
Studio Duo includes the LAME 3.100 MP3 encoder, licensed under LGPL-2.0-or-later.
Project: https://lame.sourceforge.io/
Unmodified upstream source: https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
SHA-256: ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
The bundled decoder (mpglib), executables, and assembly routines are not built.
The accompanying lame-3.100-source.tar.gz contains the complete library source,
and lame-config.h records the scalar encoder build configuration.
The accompanying studio-duo-source.tar.gz contains the application source,
build scripts, assets, tests, and documentation needed to rebuild and relink.
Extract the application archive into an empty directory and follow README.md
or docs/development.md. Set FETCHCONTENT_SOURCE_DIR_STUDIO_DUO_LAME to the
extracted library directory to rebuild with a modified encoder.
The application source is also published at https://github.com/mbianchidev/studio-duo.
See lame-COPYING.txt for the complete library license.
]=] @ONLY)
set(STUDIO_DUO_LAME_LICENSE_FILES
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-COPYING.txt"
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-NOTICE.txt"
    "${STUDIO_DUO_LAME_CONFIG_DIR}/lame-config.h"
    ${STUDIO_DUO_SOURCE_ARCHIVES}
)
if(APPLE)
    set_source_files_properties(
        ${STUDIO_DUO_LAME_LICENSE_FILES}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/licenses"
    )
    target_sources(StudioDuo PRIVATE ${STUDIO_DUO_LAME_LICENSE_FILES})
else()
    add_custom_command(
        TARGET StudioDuo POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:StudioDuo>/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${STUDIO_DUO_LAME_LICENSE_FILES} "$<TARGET_FILE_DIR:StudioDuo>/licenses"
        VERBATIM
    )
endif()

target_sources(
    StudioDuo
    PRIVATE
        src/Main.cpp
        src/audio/AudioDeviceProbe.cpp
        src/audio/AudioDeviceProbe.h
        src/audio/StudioAudioDeviceManager.cpp
        src/audio/StudioAudioDeviceManager.h
        src/audio/StudioAudioEngine.cpp
        src/audio/StudioAudioEngine.h
        src/audio/RecordingWaveform.h
        src/logging/StudioLogger.cpp
        src/logging/StudioLogger.h
        src/plugin_host/PluginBrowserComponent.cpp
        src/plugin_host/PluginBrowserComponent.h
        src/plugin_host/PluginBridgeWorker.cpp
        src/plugin_host/PluginBridgeWorker.h
        src/plugin_host/PluginScanWorker.cpp
        src/plugin_host/PluginScanWorker.h
        src/render/RenderEngine.cpp
        src/render/RenderEngine.h
        src/update/UpdateInstaller.h
        src/update/UpdateManifest.cpp
        src/update/UpdateManifest.h
        src/update/UpdateService.cpp
        src/update/UpdateService.h
        src/update/UpdateSettingsComponent.cpp
        src/update/UpdateSettingsComponent.h
        src/ui/MainComponent.cpp
        src/ui/MainComponent.h
        src/ui/AudioExportOptionsComponent.cpp
        src/ui/AudioExportOptionsComponent.h
        src/ui/TransportSettingsComponent.cpp
        src/ui/TransportSettingsComponent.h
        src/ui/NumericInput.h
        src/ui/MainWindowSizing.cpp
        src/ui/MainWindowSizing.h
        src/ui/MasteringWorkspaceComponent.cpp
        src/ui/MasteringWorkspaceComponent.h
        src/ui/MidiEditorComponent.cpp
        src/ui/MidiEditorComponent.h
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
        src/ui/StudioIconButton.cpp
        src/ui/StudioIconButton.h
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
            src/update/MacUpdateInstaller.mm
    )
elseif(WIN32)
    target_sources(
        StudioDuo
        PRIVATE
            src/platform/WindowsCrashHandler.cpp
            src/platform/WindowsCrashHandler.h
            src/update/WindowsUpdateInstaller.cpp
    )
else()
    target_sources(
        StudioDuo
        PRIVATE
            src/update/UnsupportedUpdateInstaller.cpp
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
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_ARA=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        STUDIO_DUO_VERSION="${PROJECT_VERSION}"
        STUDIO_DUO_UPDATE_MANIFEST_URL="https://github.com/mbianchidev/studio-duo/releases/latest/download/update-manifest.json"
)

if(WIN32)
    target_compile_definitions(StudioDuo PRIVATE JUCE_ASIO=1)
endif()

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
        SampleRate::samplerate
        StudioDuoBrandData
        StudioDuoDawProjectSchemaData
        StudioDuoMp3Encoder
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

set(STUDIO_DUO_BUNDLED_PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND STUDIO_DUO_BUNDLED_PLUGIN_FORMATS AU)
endif()

function(studio_duo_add_bundled_plugin
         target
         product_name
         bundle_suffix
         plugin_code
         processor_definition
         is_synth
         needs_midi)
    juce_add_plugin(
        ${target}
        COMPANY_NAME "Studio Duo"
        COMPANY_WEBSITE "https://github.com/mbianchidev/studio-duo"
        COMPANY_EMAIL "opensource@studio-duo.invalid"
        BUNDLE_ID "${STUDIO_DUO_BUNDLE_ID}.${bundle_suffix}"
        PLUGIN_MANUFACTURER_CODE StDu
        PLUGIN_CODE ${plugin_code}
        FORMATS ${STUDIO_DUO_BUNDLED_PLUGIN_FORMATS}
        PRODUCT_NAME "${product_name}"
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT ${needs_midi}
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    juce_generate_juce_header(${target})
    target_sources(
        ${target}
        PRIVATE
            src/devices/AmpDeviceProcessor.cpp
            src/devices/AmpDeviceProcessor.h
            src/devices/BundledPluginEntry.cpp
            src/devices/DrumDeviceProcessor.cpp
            src/devices/DrumDeviceProcessor.h
    )
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(
        ${target}
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            ${processor_definition}=1
        PUBLIC
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
    target_link_libraries(
        ${target}
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_utils
            juce::juce_audio_plugin_client
            juce::juce_dsp
    )
endfunction()

studio_duo_add_bundled_plugin(
    StudioDuoDrumComposer
    "Studio Duo Metal Drum Composer"
    "drum-composer"
    Drm1
    STUDIO_DUO_PLUGIN_DRUM
    TRUE
    TRUE
)
studio_duo_add_bundled_plugin(
    StudioDuoGuitarAmp
    "Studio Duo Guitar Amp"
    "guitar-amp"
    GAm1
    STUDIO_DUO_PLUGIN_GUITAR_AMP
    FALSE
    FALSE
)
studio_duo_add_bundled_plugin(
    StudioDuoBassAmp
    "Studio Duo Bass Amp"
    "bass-amp"
    BAm1
    STUDIO_DUO_PLUGIN_BASS_AMP
    FALSE
    FALSE
)

add_library(
    StudioDuoBundledDevicesClap
    MODULE
        src/devices/AmpDeviceProcessor.cpp
        src/devices/AmpDeviceProcessor.h
        src/devices/BundledClapPlugin.cpp
        src/devices/DrumDeviceProcessor.cpp
        src/devices/DrumDeviceProcessor.h
)
target_include_directories(
    StudioDuoBundledDevicesClap
    PRIVATE
        src
)
target_compile_definitions(
    StudioDuoBundledDevicesClap
    PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        STUDIO_DUO_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(
    StudioDuoBundledDevicesClap
    PRIVATE
        clap
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
)
set_target_properties(
    StudioDuoBundledDevicesClap
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "StudioDuoBundledDevices"
)
if(APPLE)
    set_target_properties(
        StudioDuoBundledDevicesClap
        PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION clap
            MACOSX_BUNDLE_GUI_IDENTIFIER
                "${STUDIO_DUO_BUNDLE_ID}.bundled-devices-clap"
            MACOSX_BUNDLE_BUNDLE_NAME "StudioDuoBundledDevices"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
elseif(WIN32)
    set_target_properties(
        StudioDuoBundledDevicesClap
        PROPERTIES
            SUFFIX ".clap"
    )
endif()

if(STUDIO_DUO_BUILD_TESTS)
    enable_testing()

    add_executable(
        StudioDuoDdpEncoderFixture
        tests/fixtures/DdpEncoderFixture.cpp
    )

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

    juce_add_plugin(
        StudioDuoRoutingFixture
        COMPANY_NAME "Studio Duo"
        COMPANY_WEBSITE "https://github.com/mbianchidev/studio-duo"
        COMPANY_EMAIL "opensource@studio-duo.invalid"
        BUNDLE_ID "dev.mbianchi.studioduo.routing-fixture"
        PLUGIN_MANUFACTURER_CODE StDu
        PLUGIN_CODE RtFt
        FORMATS VST3
        PRODUCT_NAME "Studio Duo Routing Fixture"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    juce_generate_juce_header(StudioDuoRoutingFixture)
    target_sources(
        StudioDuoRoutingFixture
        PRIVATE
            tests/fixtures/RoutingTestPlugin.cpp
    )
    target_compile_definitions(
        StudioDuoRoutingFixture
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
        PUBLIC
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
    target_link_libraries(
        StudioDuoRoutingFixture
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
            tests/MidiTests.cpp
            tests/RoutingModelTests.cpp
            tests/RoutingEngineTests.cpp
            tests/RoutingUiModelTests.cpp
            tests/PluginFormatTests.cpp
            tests/PluginSearchPathTests.cpp
            tests/PluginRecoveryTests.cpp
            tests/AutomationTests.cpp
            tests/DeviceTests.cpp
            tests/BundledDeviceTests.cpp
            tests/AudioDeviceProbeTests.cpp
            tests/LoggingTests.cpp
            tests/WindowsCrashHandlerTests.cpp
            tests/ReampSnapshotTests.cpp
            tests/RenderEngineTests.cpp
            tests/AudioExportOptionsTests.cpp
            tests/AudioExportTests.cpp
            tests/MasteringTests.cpp
            tests/PluginCompatibilityTests.cpp
            tests/ProjectMigrationTests.cpp
            tests/WindowSizingTests.cpp
            tests/TransportTests.cpp
            tests/TransportEditingTests.cpp
            tests/SectionTransportTests.cpp
            tests/TransportSettingsTests.cpp
            tests/UpdateTests.cpp
            tests/UiIconTests.cpp
            tests/TimelineMarkerTests.cpp
            tests/DawProjectTests.cpp
            src/ui/TransportSettingsComponent.cpp
            src/ui/TransportSettingsComponent.h
            src/ui/NumericInput.h
            src/ui/AudioExportOptionsComponent.cpp
            src/ui/AudioExportOptionsComponent.h
            src/ui/RoutingUiModel.cpp
            src/ui/RoutingUiModel.h
            src/ui/StudioIconButton.cpp
            src/ui/StudioIconButton.h
            src/ui/TimelineComponent.cpp
            src/ui/TimelineComponent.h
            src/ui/MainWindowSizing.cpp
            src/ui/MainWindowSizing.h
            src/ui/MidiEditorComponent.cpp
            src/ui/MidiEditorComponent.h
            src/audio/StudioAudioDeviceManager.cpp
            src/audio/StudioAudioDeviceManager.h
            src/audio/AudioDeviceProbe.cpp
            src/audio/AudioDeviceProbe.h
            src/audio/StudioAudioEngine.cpp
            src/audio/StudioAudioEngine.h
            src/logging/StudioLogger.cpp
            src/logging/StudioLogger.h
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
            src/plugin_host/PluginSearchPaths.cpp
            src/plugin_host/PluginSearchPaths.h
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
            src/devices/AmpDeviceProcessor.cpp
            src/devices/AmpDeviceProcessor.h
            src/devices/DeviceRegistry.cpp
            src/devices/DeviceRegistry.h
            src/devices/DrumDeviceProcessor.cpp
            src/devices/DrumDeviceProcessor.h
            src/devices/UtilityDeviceProcessor.cpp
            src/devices/UtilityDeviceProcessor.h
            src/render/RenderEngine.cpp
            src/render/RenderEngine.h
            src/update/UpdateManifest.cpp
            src/update/UpdateManifest.h
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
            JUCE_USE_MP3AUDIOFORMAT=1
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_ARA=1
            JUCE_MODAL_LOOPS_PERMITTED=1
            STUDIO_DUO_TESTING=1
            STUDIO_DUO_VERSION="${PROJECT_VERSION}"
            STUDIO_DUO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(WIN32)
        target_compile_definitions(StudioDuoTests PRIVATE JUCE_ASIO=1)
        target_sources(
            StudioDuoTests
            PRIVATE
                src/platform/WindowsCrashHandler.cpp
                src/platform/WindowsCrashHandler.h
        )
    endif()
    if(APPLE)
        target_compile_definitions(
            StudioDuoTests
            PRIVATE
                STUDIO_DUO_ARA_FIXTURE_PATH="$<TARGET_BUNDLE_DIR:StudioDuoAraFixture_VST3>"
                STUDIO_DUO_CLAP_FIXTURE_PATH="$<TARGET_BUNDLE_DIR:StudioDuoClapFixture>"
                STUDIO_DUO_DDP_FIXTURE_PATH="$<TARGET_FILE:StudioDuoDdpEncoderFixture>"
                STUDIO_DUO_ROUTING_FIXTURE_PATH="$<TARGET_BUNDLE_DIR:StudioDuoRoutingFixture_VST3>"
                STUDIO_DUO_BRIDGE_WORKER_PATH="$<TARGET_FILE:StudioDuo>"
        )
    else()
        target_compile_definitions(
            StudioDuoTests
            PRIVATE
                STUDIO_DUO_ARA_FIXTURE_PATH="$<TARGET_FILE:StudioDuoAraFixture_VST3>"
                STUDIO_DUO_CLAP_FIXTURE_PATH="$<TARGET_FILE:StudioDuoClapFixture>"
                STUDIO_DUO_DDP_FIXTURE_PATH="$<TARGET_FILE:StudioDuoDdpEncoderFixture>"
                STUDIO_DUO_ROUTING_FIXTURE_PATH="$<TARGET_FILE:StudioDuoRoutingFixture_VST3>"
                STUDIO_DUO_BRIDGE_WORKER_PATH="$<TARGET_FILE:StudioDuo>"
        )
    endif()
    add_dependencies(
        StudioDuoTests
        StudioDuo
        StudioDuoDdpEncoderFixture
        StudioDuoAraFixture_VST3
        StudioDuoClapFixture
        StudioDuoRoutingFixture_VST3
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
            SampleRate::samplerate
            StudioDuoDawProjectSchemaData
            StudioDuoMp3Encoder
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )

    add_test(NAME StudioDuoTests COMMAND StudioDuoTests)
    if(WIN32)
        add_test(NAME StudioDuoStartup COMMAND StudioDuo --startup-self-test)
        set_tests_properties(StudioDuoStartup PROPERTIES TIMEOUT 30)
    endif()
endif()
