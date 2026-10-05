cmake_minimum_required(VERSION 3.15)

project(PhaseAlign VERSION 0.1.0)   # release.yml greps this line for the version; keep the format

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment version")
# Universal builds are produced in CI by passing -DCMAKE_OSX_ARCHITECTURES (see release.yml).

# -Wextra is a GCC/Clang flag; MSVC's cl.exe misparses it as the /W switch (error D8021).
if (MSVC)
    set(PA_WARNING_FLAGS /W4)
else()
    set(PA_WARNING_FLAGS -Wall -Wextra)
endif()

add_subdirectory(libs/JUCE)

juce_add_plugin(PhaseAlign
    COMPANY_NAME "Leigh Pierce"
    BUNDLE_ID com.leighpierce.phasealign
    PLUGIN_MANUFACTURER_CODE Lprc   # 4 chars, one uppercase
    PLUGIN_CODE PhAl                # 4 chars, one uppercase, unique per plugin
    FORMATS AU VST3
    PRODUCT_NAME "Phase Align"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect
)

target_sources(PhaseAlign
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
)

target_include_directories(PhaseAlign PRIVATE src)

target_compile_definitions(PhaseAlign
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PhaseAlign
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_compile_options(PhaseAlign PRIVATE ${PA_WARNING_FLAGS})
