cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SculptLead VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SCULPT_LEAD_CORE_ONLY "Build the independent DSP tests without JUCE" OFF)
option(SCULPT_LEAD_BUILD_QA "Build the processor/state tests and preview utility" OFF)
option(SCULPT_LEAD_ENABLE_LTO "Enable link-time optimisation for the plug-in" OFF)
set(SCULPT_JUCE_SOURCE_DIR "" CACHE PATH "Optional local JUCE 8.0.4 source directory")

include(CTest)
if(BUILD_TESTING)
  add_executable(SculptLeadCoreTests Tests/lead_engine_test.cpp)
  target_include_directories(SculptLeadCoreTests PRIVATE Source)
  if(MSVC)
    target_compile_options(SculptLeadCoreTests PRIVATE /W4)
  else()
    target_compile_options(SculptLeadCoreTests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME lead-core COMMAND SculptLeadCoreTests)
endif()
if(SCULPT_LEAD_CORE_ONLY)
  return()
endif()

if(SCULPT_JUCE_SOURCE_DIR)
  add_subdirectory("${SCULPT_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

file(GLOB_RECURSE SCULPT_LEAD_WAVS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Samples/*.wav")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Samples/manifest.json" OR NOT SCULPT_LEAD_WAVS)
  message(FATAL_ERROR "The complete Samples/manifest.json and WAV sample bank are required. Upload the whole source package, including Samples.")
endif()
set(sculpt_sample_basenames "manifest.json")
foreach(sculpt_sample IN LISTS SCULPT_LEAD_WAVS)
  get_filename_component(sculpt_basename "${sculpt_sample}" NAME)
  if(sculpt_basename IN_LIST sculpt_sample_basenames)
    message(FATAL_ERROR "Duplicate embedded sample filename: ${sculpt_basename}")
  endif()
  list(APPEND sculpt_sample_basenames "${sculpt_basename}")
endforeach()

juce_add_binary_data(SculptLeadSamples
  HEADER_NAME LeadSampleData.h
  NAMESPACE LeadSamples
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Samples/manifest.json" ${SCULPT_LEAD_WAVS})
set_target_properties(SculptLeadSamples PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(sculpt_lead_sources
  Source/LeadEngine.h
  Source/PluginProcessor.h
  Source/PluginProcessor.cpp
  Source/PluginEditor.h
  Source/PluginEditor.cpp)

set(sculpt_lead_definitions
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_USE_FLAC=0
  JUCE_USE_OGGVORBIS=0
  JUCE_USE_MP3AUDIOFORMAT=0)

juce_add_plugin(SculptLead
  COMPANY_NAME "Sculpt"
  BUNDLE_ID "com.sculpt.lead"
  PRODUCT_NAME "Sculpt Lead"
  PLUGIN_MANUFACTURER_CODE Scpt
  PLUGIN_CODE Sled
  FORMATS VST3
  VST3_CATEGORIES Instrument Synth
  IS_SYNTH TRUE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SculptLead PRIVATE ${sculpt_lead_sources})
target_compile_definitions(SculptLead PUBLIC ${sculpt_lead_definitions})
target_link_libraries(SculptLead
  PRIVATE SculptLeadSamples juce::juce_audio_processors juce::juce_audio_formats
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(SCULPT_LEAD_ENABLE_LTO)
  target_link_libraries(SculptLead PUBLIC juce::juce_recommended_lto_flags)
endif()

if(SCULPT_LEAD_BUILD_QA)
  function(sculpt_lead_qa_target target source)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")
    target_sources(${target} PRIVATE ${source} ${sculpt_lead_sources})
    target_include_directories(${target} PRIVATE Source)
    target_compile_definitions(${target} PRIVATE ${sculpt_lead_definitions}
      JUCE_STANDALONE_APPLICATION=1
      JucePlugin_Name="Sculpt Lead"
      JucePlugin_IsSynth=1
      JucePlugin_WantsMidiInput=1
      JucePlugin_ProducesMidiOutput=0
      JucePlugin_IsMidiEffect=0)
    target_link_libraries(${target} PRIVATE SculptLeadSamples
      juce::juce_audio_processors juce::juce_audio_formats
      juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  endfunction()
  # Compile the processor directly: linking the plug-in shared target into a
  # standalone test would import conflicting JUCE application definitions.
  sculpt_lead_qa_target(SculptLeadStateTests Tests/plugin_state_test.cpp)
  sculpt_lead_qa_target(SculptLeadPreview tools/render_factory_preview.cpp)
  if(BUILD_TESTING)
    add_test(NAME lead-state COMMAND SculptLeadStateTests)
  endif()
endif()
