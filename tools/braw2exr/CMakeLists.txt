# braw2exr - Blackmagic RAW -> OpenEXR helper for the Natron fork (MIT).
#
# The Blackmagic RAW SDK is NOT in this repository: download it (free) from
# https://www.blackmagicdesign.com/developer/products/braw and give its
# platform folder (the one that contains Include/), e.g.
#   cmake -B build -DBRAW_SDK_DIR="C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win"
cmake_minimum_required(VERSION 3.16)
project(braw2exr CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BRAW_SDK_DIR "" CACHE PATH "Blackmagic RAW SDK platform folder (contains Include/)")

find_path(BRAW_INCLUDE_DIR BlackmagicRawAPI.h
          HINTS "${BRAW_SDK_DIR}" ENV BRAW_SDK_DIR
          PATH_SUFFIXES Include include
          PATHS "C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win"
                "C:/Program Files/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win"
                "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac"
                "/usr/lib64/blackmagic/BlackmagicRAWSDK/Linux"
                "/usr/lib/blackmagic/BlackmagicRAWSDK/Linux")
if(NOT BRAW_INCLUDE_DIR)
    message(FATAL_ERROR "BlackmagicRawAPI.h not found: install the Blackmagic RAW SDK and pass -DBRAW_SDK_DIR=<SDK>/Win (or Mac / Linux)")
endif()
message(STATUS "Blackmagic RAW SDK include: ${BRAW_INCLUDE_DIR}")

set(SOURCES braw2exr.cpp exr_write.h)
if(WIN32)
    # COM GUIDs (CLSID_CBlackmagicRawFactory, IID_...) come with the SDK as a _i.c file
    file(GLOB BRAW_GUIDS "${BRAW_INCLUDE_DIR}/*_i.c")
    list(APPEND SOURCES ${BRAW_GUIDS})
else()
    # Linux / macOS: the SDK's dispatch source loads the library at run time
    find_file(BRAW_DISPATCH BlackmagicRawAPIDispatch.cpp HINTS "${BRAW_INCLUDE_DIR}")
    if(BRAW_DISPATCH)
        list(APPEND SOURCES "${BRAW_DISPATCH}")
    endif()
    get_filename_component(_sdk "${BRAW_INCLUDE_DIR}" DIRECTORY)
    set(BRAW_LIBRARY_DIR "${_sdk}/Libraries" CACHE PATH "Folder of the Blackmagic RAW runtime library")
endif()

add_executable(braw2exr ${SOURCES})
target_include_directories(braw2exr PRIVATE "${BRAW_INCLUDE_DIR}")
if(WIN32)
    target_link_libraries(braw2exr PRIVATE ole32 oleaut32 uuid)
    if(MINGW)
        target_link_options(braw2exr PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    target_compile_definitions(braw2exr PRIVATE BRAW2EXR_LIBRARY_PATH="${BRAW_LIBRARY_DIR}")
    target_link_libraries(braw2exr PRIVATE ${CMAKE_DL_LIBS} pthread)
    if(APPLE)
        target_link_libraries(braw2exr PRIVATE "-framework CoreFoundation")
    endif()
endif()

install(TARGETS braw2exr RUNTIME DESTINATION bin)
