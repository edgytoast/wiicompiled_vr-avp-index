# WiiCompiled for Apple Vision Pro: the SwiftUI app around the game runtime.
#
# One Xcode project (the only generator that signs, embeds Swift and produces a
# visionOS bundle) holding the runtime tree (../runtime, which on visionOS
# yields the products as static libraries) and this app, which links one
# product with -force_load and drives it through
# runtime/include/platform/visionos/visionos_host.h.
#
#   cmake -S visionos -B build-visionos -G Xcode \
#       -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
#       -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
#       -DAURORA_DAWN_PACKAGE_URL=file:///.../dawn-visionos-arm64.tar.gz \
#       -DMKW_VISIONOS_TEAM=<Apple developer team id>
#   cmake --build build-visionos --config Release --target WiiCompiledVision
#
# visionos/Build-VisionOS.sh runs both, after building Dawn for the SDK
# (Build-VisionOSDawn.sh). The translation (generated/) must exist already, as
# for any other platform: docs/building-macos.md, steps 1 to 5.
cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=visionOS (and -DCMAKE_OSX_SYSROOT=xros or xrsimulator)")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "The visionOS app needs the Xcode generator (-G Xcode): Swift, signing and the bundle come from it")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 2.0 CACHE STRING "" FORCE)
endif()
# The runtime only builds Release; Xcode is multi-config, so it is the one configuration.
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

project(WiiCompiledVision LANGUAGES C CXX OBJC OBJCXX Swift ASM)

set(MKW_VISIONOS_PRODUCT base CACHE STRING "Which game the app carries: base (WiiCompiled) or retro_rewind")
set_property(CACHE MKW_VISIONOS_PRODUCT PROPERTY STRINGS base retro_rewind)
set(MKW_VISIONOS_TEAM "" CACHE STRING "Apple developer team id for automatic signing (empty: sign in Xcode)")
set(MKW_VISIONOS_BUNDLE_ID "org.wiicompiled.vision" CACHE STRING "Bundle identifier of the app")
set(MKW_VISIONOS_APP_NAME "WiiCompiled Vision" CACHE STRING "Name shown on the Home View")

# Xcode signs every Mach-O it produces on the iOS family, including the dependency
# libraries' by-products (libpng's dylib, for one), and refuses without a team. Only the
# app bundle is signed; the libraries are linked into it and need no identity of their own.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
# Nothing in the app is loaded dynamically; skip the shared flavours the dependencies offer.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)

# The runtime tree. Its own CMakeLists detects visionOS from CMAKE_SYSTEM_NAME,
# turns OpenXR on with the CompositorServices provider and makes the products
# static libraries (runtime/cmake/PublicProducts.cmake).
set(MKW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../runtime" runtime)

if(MKW_VISIONOS_PRODUCT STREQUAL "retro_rewind")
    if(NOT TARGET RetroRewind)
        message(FATAL_ERROR "MKW_VISIONOS_PRODUCT=retro_rewind but the translation has no Retro Rewind shards")
    endif()
    set(MKW_VISIONOS_GAME_TARGET RetroRewind)
    set(MKW_VISIONOS_GAME_TITLE "Retro Rewind")
    set(MKW_VISIONOS_SWIFT_CONDITIONS "MKW_VISIONOS_RETRO_REWIND")
elseif(MKW_VISIONOS_PRODUCT STREQUAL "base")
    set(MKW_VISIONOS_GAME_TARGET WiiCompiled)
    set(MKW_VISIONOS_GAME_TITLE "WiiCompiled")
    set(MKW_VISIONOS_SWIFT_CONDITIONS "")
else()
    message(FATAL_ERROR "MKW_VISIONOS_PRODUCT must be base or retro_rewind")
endif()

set(MKW_VISIONOS_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/App")
set(MKW_VISIONOS_APP_SOURCES
    "${MKW_VISIONOS_APP_DIR}/WiiCompiledVisionApp.swift"
    "${MKW_VISIONOS_APP_DIR}/GameModel.swift"
    "${MKW_VISIONOS_APP_DIR}/LauncherView.swift"
    "${MKW_VISIONOS_APP_DIR}/ImmersiveGameView.swift")

add_executable(WiiCompiledVision MACOSX_BUNDLE
    ${MKW_VISIONOS_APP_SOURCES}
    "${MKW_VISIONOS_APP_DIR}/BridgingHeader.h")
set_target_properties(WiiCompiledVision PROPERTIES
    OUTPUT_NAME "WiiCompiledVision"
    MACOSX_BUNDLE_INFO_PLIST "${MKW_VISIONOS_APP_DIR}/Info.plist"
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MKW_VISIONOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_NAME "WiiCompiledVision"
    XCODE_ATTRIBUTE_MKW_APP_DISPLAY_NAME "${MKW_VISIONOS_APP_NAME}"
    XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
    # 7 is the Vision device family.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "xros xrsimulator"
    XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${MKW_VISIONOS_APP_DIR}/BridgingHeader.h"
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
    XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
    XCODE_ATTRIBUTE_ENABLE_PREVIEWS NO
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED YES
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MKW_VISIONOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${MKW_VISIONOS_APP_DIR}/WiiCompiledVision.entitlements"
    # The game's static initialisers and the OpenXR provider's exported xr*
    # symbols come with -force_load below; dead stripping the rest is fine.
    XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "-O"
    XCODE_ATTRIBUTE_SWIFT_ACTIVE_COMPILATION_CONDITIONS "${MKW_VISIONOS_SWIFT_CONDITIONS}"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_GENERATE_ASSET_SYMBOLS NO)
target_include_directories(WiiCompiledVision PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include")
# The product archive carries the runtime's objects (mkw_runtime_common is consumed as objects) and
# the registration shards, some of which register themselves from static initialisers no symbol
# refers to; the whole archive is loaded. Its link interface brings SDL3, Dawn, Aurora, the
# provider and the frameworks along.
target_link_libraries(WiiCompiledVision PRIVATE ${MKW_VISIONOS_GAME_TARGET} "c++")
target_link_options(WiiCompiledVision PRIVATE "LINKER:-force_load,$<TARGET_FILE:${MKW_VISIONOS_GAME_TARGET}>")

# The runtime's read-only resources, where RuntimePlatform::ExecutableDirectory() looks for them
# (the bundle root is the resource directory on the iOS family): the first-run NAND payload, the
# DSP coefficient ROM and the transferable pipeline cache. The Ninja products copy the same files
# next to the executable (mkw_configure_product), which a static library cannot.
set(MKW_VISIONOS_RUNTIME_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/assets")
# They go through Xcode's own Copy Bundle Resources phase, which runs before the bundle
# is sealed; a post-build copy lands after code signing and invalidates the signature.
# The bootstrap folder is staged under the name the runtime looks for and added as a
# folder reference (a directory source), which Xcode copies whole.
set(MKW_VISIONOS_STAGED_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/staged-resources")
file(REMOVE_RECURSE "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap")
file(COPY "${MKW_VISIONOS_RUNTIME_ASSETS}/wii/" DESTINATION "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap")
set(MKW_VISIONOS_BUNDLED_RESOURCES
    "${MKW_VISIONOS_RUNTIME_ASSETS}/dsp/dsp_coef.bin"
    "${MKW_VISIONOS_RUNTIME_ASSETS}/pipeline/initial_pipeline_cache.db"
    "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap")
target_sources(WiiCompiledVision PRIVATE ${MKW_VISIONOS_BUNDLED_RESOURCES})
set_source_files_properties(${MKW_VISIONOS_BUNDLED_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
    HEADER_FILE_ONLY ON)

message(STATUS "visionOS app: ${MKW_VISIONOS_APP_NAME} carrying ${MKW_VISIONOS_GAME_TITLE} (${MKW_VISIONOS_GAME_TARGET})")
