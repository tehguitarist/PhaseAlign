cmake_minimum_required(VERSION 3.15)

project(PhaseAlign VERSION 1.0.0)   # release.yml greps this line for the version; keep the format

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment version")
# macOS ships two separate builds, not a universal binary: arm64, the default here and locally, and x86_64
# ("Intel"), which release.yml cross-compiles with -DCMAKE_OSX_ARCHITECTURES=x86_64.

option(PA_BUILD_TESTS "Build the unit tests (tests/, Catch2 v3)" ON)
option(PA_STANDALONE "Also build a Standalone app, for checking the UI without a host" OFF)
option(PA_GENERIC_FFT_TESTS "Build PhaseAlignDspTests with PFFFT on macOS too (no vDSP), to benchmark it" OFF)

# -Wextra is a GCC/Clang flag; MSVC's cl.exe misparses it as the /W switch (error D8021).
if (MSVC)
    set(PA_WARNING_FLAGS /W4)
else()
    set(PA_WARNING_FLAGS -Wall -Wextra)
endif()

add_subdirectory(libs/JUCE)

# PFFFT (libs/pffft, a submodule; BSD-style licence in libs/pffft/LICENSE.txt): the real FFT where vDSP isn't available
# (src/dsp/RealFft.h), and its double-precision core for ANALYSE's offline search (src/analyse/Search.cpp) everywhere;
# NEON on ARM, SSE on x86 (the headers pick).
add_library(pa_pffft STATIC libs/pffft/src/pffft.c libs/pffft/src/pffft_double.c libs/pffft/src/pffft_common.c)
target_include_directories(pa_pffft
    PUBLIC libs/pffft/include
    PRIVATE libs/pffft/src libs/pffft/include/pffft)
target_compile_definitions(pa_pffft PUBLIC PFFFT_STATIC_DEFINE PRIVATE _USE_MATH_DEFINES PFFFT_ENABLE_NEON)
set_target_properties(pa_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

# UI artwork (built from ui/ by tools/build_assets.sh) and fonts. Resources are looked up by file name
# (src/ui/Assets.cpp), so new files only need to land in these folders.
file(GLOB PA_ASSET_FILES CONFIGURE_DEPENDS assets/images/*.png assets/fonts/*.ttf)
juce_add_binary_data(PhaseAlignAssets SOURCES ${PA_ASSET_FILES})

# Plugin sources, shared by the plugin and the test runner.
set(PA_SOURCES
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/params/Parameters.cpp
    src/meter/CorrelationAnalyser.cpp
    src/analyse/Search.cpp
    src/analyse/Session.cpp
    src/ui/AnalyseScreen.cpp
    src/ui/Assets.cpp
    src/ui/ImageKnob.cpp
    src/ui/MeterScreen.cpp
    src/ui/PanelButton.cpp
    src/ui/Readout.cpp
    src/ui/HelpButton.cpp
    src/ui/ScaleLabel.cpp
    src/ui/ToggleSwitch3.cpp
)

set(PA_FORMATS AU VST3)
if (PA_STANDALONE)
    list(APPEND PA_FORMATS Standalone)
endif()

juce_add_plugin(PhaseAlign
    COMPANY_NAME "Leigh Pierce"
    BUNDLE_ID com.leighpierce.phasealign
    PLUGIN_MANUFACTURER_CODE Lprc   # 4 chars, one uppercase
    PLUGIN_CODE PhAl                # 4 chars, one uppercase, unique per plugin
    FORMATS ${PA_FORMATS}
    PRODUCT_NAME "Phase Align"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect
)

target_sources(PhaseAlign PRIVATE ${PA_SOURCES})

target_include_directories(PhaseAlign PRIVATE src)

target_compile_definitions(PhaseAlign
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PhaseAlign
    PRIVATE
        PhaseAlignAssets
        pa_pffft
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_compile_options(PhaseAlign PRIVATE ${PA_WARNING_FLAGS})

# --- Tests ---------------------------------------------------------------------------------------
# One Catch2 runner, built as a JUCE console app from the same sources as the plugin (the state and
# editor tests need the real processor and editor). It compiles its own copy of the JUCE modules, so it
# roughly doubles a clean build; configure with -DPA_BUILD_TESTS=OFF to skip it. Run with ctest.
if (PA_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    juce_add_console_app(PhaseAlignTests PRODUCT_NAME "PhaseAlignTests")
    target_sources(PhaseAlignTests PRIVATE
        ${PA_SOURCES}
        tests/main.cpp
        tests/ParameterTests.cpp
        tests/StateTests.cpp
        tests/EditorTests.cpp
        tests/InteractionTests.cpp
        tests/ProcessorTests.cpp
        tests/DimmingTests.cpp
        tests/RobustnessTests.cpp
        tests/AnalyseFlowTests.cpp
    )
    target_include_directories(PhaseAlignTests PRIVATE src tests)
    target_compile_definitions(PhaseAlignTests PRIVATE
        _USE_MATH_DEFINES                     # M_PI in the tests, on MSVC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1          # the idle-repaint test runs the message loop
        JucePlugin_Name="Phase Align"
    )
    target_link_libraries(PhaseAlignTests PRIVATE
        PhaseAlignAssets
        pa_pffft
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        Catch2::Catch2
    )
    target_compile_options(PhaseAlignTests PRIVATE ${PA_WARNING_FLAGS})
    # Exact float comparisons are deliberate in the tests (Catch's CHECK(a == b) on stored values).
    set(PA_DSP_TEST_SOURCES
        tests/dsp/main.cpp
        tests/dsp/ChainTests.cpp
        tests/dsp/PhaseTests.cpp
        tests/dsp/GoldenTests.cpp
        tests/dsp/MeterTests.cpp
        tests/dsp/AnalyseTests.cpp
    )
    set_source_files_properties(tests/ParameterTests.cpp tests/StateTests.cpp tests/EditorTests.cpp
        tests/InteractionTests.cpp tests/ProcessorTests.cpp tests/DimmingTests.cpp tests/RobustnessTests.cpp
        tests/AnalyseFlowTests.cpp ${PA_DSP_TEST_SOURCES}
        PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-float-equal>")

    add_test(NAME PhaseAlignTests COMMAND PhaseAlignTests)

    # The light DSP runner: src/dsp and src/meter with juce_dsp only, no GUI modules or plugin sources,
    # so it builds in a fraction of the time. It counts global operator new to check the real-time paths.
    juce_add_console_app(PhaseAlignDspTests PRODUCT_NAME "PhaseAlignDspTests")
    target_sources(PhaseAlignDspTests PRIVATE ${PA_DSP_TEST_SOURCES} src/meter/CorrelationAnalyser.cpp
        src/analyse/Search.cpp)
    target_include_directories(PhaseAlignDspTests PRIVATE src tests/dsp)
    target_compile_definitions(PhaseAlignDspTests PRIVATE
        PA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden" # reference/golden.py writes these
        _USE_MATH_DEFINES                                        # M_PI in the tests, on MSVC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
    )
    target_link_libraries(PhaseAlignDspTests PRIVATE
        pa_pffft
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        Catch2::Catch2
    )
    target_compile_options(PhaseAlignDspTests PRIVATE ${PA_WARNING_FLAGS})
    add_test(NAME PhaseAlignDspTests COMMAND PhaseAlignDspTests)

    # The DSP runner without vDSP on macOS: the FFT engine Windows and Linux use (PFFFT), to benchmark it here. Only
    # juce_dsp loses vDSP (juce_audio_basics needs it on macOS), so this build directory is for benchmarks only.
    if (PA_GENERIC_FFT_TESTS)
        target_compile_definitions(PhaseAlignDspTests PRIVATE PA_REAL_FFT_NO_VDSP=1)
        set_source_files_properties(libs/JUCE/modules/juce_dsp/juce_dsp.mm libs/JUCE/modules/juce_dsp/juce_dsp.cpp
            PROPERTIES COMPILE_DEFINITIONS JUCE_USE_VDSP_FRAMEWORK=0)
    endif()
endif()
