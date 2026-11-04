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

set(_braw_hints
    "${BRAW_SDK_DIR}" "$ENV{BRAW_SDK_DIR}"
    "C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win"
    "C:/Program Files/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win"
    "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac"
    "/usr/lib64/blackmagic/BlackmagicRAWSDK/Linux"
    "/usr/lib/blackmagic/BlackmagicRAWSDK/Linux")
if(WIN32)
    # The Windows SDK ships the API as an IDL file (no .h): generate the header
    # and the GUID file into the build folder with widl (MSYS2 package
    # mingw-w64-ucrt-x86_64-tools-git) - or MIDL with Visual Studio.
    find_file(BRAW_IDL BlackmagicRawAPI.idl HINTS ${_braw_hints} PATH_SUFFIXES Include include NO_DEFAULT_PATH)
    if(NOT BRAW_IDL)
        message(FATAL_ERROR "BlackmagicRawAPI.idl not found: install the Blackmagic RAW SDK and pass -DBRAW_SDK_DIR=<SDK>/Win")
    endif()
    get_filename_component(BRAW_INCLUDE_DIR "${BRAW_IDL}" DIRECTORY)
    set(BRAW_GEN_DIR "${CMAKE_BINARY_DIR}/braw_generated")
    file(MAKE_DIRECTORY "${BRAW_GEN_DIR}")
    if(NOT EXISTS "${BRAW_GEN_DIR}/BlackmagicRawAPI.h")
        find_program(WIDL NAMES widl x86_64-w64-mingw32-widl)
        if(NOT WIDL)
            message(FATAL_ERROR "widl not found. In MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-tools-git")
        endif()
        get_filename_component(_widl_dir "${WIDL}" DIRECTORY)
        set(_widl_inc -I "${_widl_dir}/../include" -I "${BRAW_INCLUDE_DIR}")
        # widl (unlike MIDL) rejects "long long" and needs stdole2.tlb for
        # importlib: compile a build-folder copy with __int64 (same type) and
        # without the importlib line (only used for type libraries).
        file(READ "${BRAW_IDL}" _idl)
        string(REPLACE "unsigned long long" "unsigned __int64" _idl "${_idl}")
        string(REPLACE "long long" "__int64" _idl "${_idl}")
        string(REGEX REPLACE "importlib[^\n]*" "" _idl "${_idl}")
        set(_gen_idl "${BRAW_GEN_DIR}/BlackmagicRawAPI.idl")
        file(WRITE "${_gen_idl}" "${_idl}")
        execute_process(COMMAND "${WIDL}" ${_widl_inc} -h -o "${BRAW_GEN_DIR}/BlackmagicRawAPI.h" "${_gen_idl}"
                        RESULT_VARIABLE _r1 ERROR_VARIABLE _e1)
        execute_process(COMMAND "${WIDL}" ${_widl_inc} -u -o "${BRAW_GEN_DIR}/BlackmagicRawAPI_i.c" "${_gen_idl}"
                        RESULT_VARIABLE _r2 ERROR_VARIABLE _e2)
        if(NOT _r1 EQUAL 0 OR NOT _r2 EQUAL 0)
            file(REMOVE "${BRAW_GEN_DIR}/BlackmagicRawAPI.h")
            message(FATAL_ERROR "widl failed on ${BRAW_IDL}:\n${_e1}\n${_e2}")
        endif()
    endif()
else()
    find_path(BRAW_INCLUDE_DIR BlackmagicRawAPI.h HINTS ${_braw_hints} PATH_SUFFIXES Include include)
    if(NOT BRAW_INCLUDE_DIR)
        message(FATAL_ERROR "BlackmagicRawAPI.h not found: install the Blackmagic RAW SDK and pass -DBRAW_SDK_DIR=<SDK>/Mac or /Linux")
    endif()
endif()
message(STATUS "Blackmagic RAW SDK include: ${BRAW_INCLUDE_DIR}")

set(SOURCES braw2exr.cpp exr_write.h)
get_filename_component(_sdk "${BRAW_INCLUDE_DIR}" DIRECTORY)
set(BRAW_LIBRARY_DIR "${_sdk}/Libraries" CACHE PATH "Folder of the Blackmagic RAW runtime library")
if(WIN32)
    list(APPEND SOURCES "${BRAW_GEN_DIR}/BlackmagicRawAPI_i.c" "${BRAW_INCLUDE_DIR}/BlackmagicRawAPIDispatch.cpp")
else()
    # Linux / macOS: the SDK's dispatch source loads the library at run time
    find_file(BRAW_DISPATCH BlackmagicRawAPIDispatch.cpp HINTS "${BRAW_INCLUDE_DIR}" NO_DEFAULT_PATH)
    if(BRAW_DISPATCH)
        list(APPEND SOURCES "${BRAW_DISPATCH}")
    endif()
endif()

add_executable(braw2exr ${SOURCES})
target_include_directories(braw2exr PRIVATE "${BRAW_INCLUDE_DIR}")
target_compile_definitions(braw2exr PRIVATE BRAW2EXR_LIBRARY_PATH="${BRAW_LIBRARY_DIR}")
if(WIN32)
    target_include_directories(braw2exr PRIVATE "${BRAW_GEN_DIR}")
    target_compile_definitions(braw2exr PRIVATE UNICODE _UNICODE MINGW_HAS_SECURE_API=1)
    target_link_libraries(braw2exr PRIVATE ole32 oleaut32 uuid)
    if(MINGW)
        target_link_options(braw2exr PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    target_link_libraries(braw2exr PRIVATE ${CMAKE_DL_LIBS} pthread)
    if(APPLE)
        target_link_libraries(braw2exr PRIVATE "-framework CoreFoundation")
    endif()
endif()

install(TARGETS braw2exr RUNTIME DESTINATION bin)
