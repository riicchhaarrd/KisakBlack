if (NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The portable target supports Linux and Emscripten")
endif()

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB_RECURSE KISAK_PORTABLE_CPP CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/DemonWare/*.cpp"
    "${SRC_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tl/*.cpp"
)
list(FILTER KISAK_PORTABLE_CPP EXCLUDE REGEX "/src/(libs|tracy|steam|nvapi|binklib|win32)/")
list(FILTER KISAK_PORTABLE_CPP EXCLUDE REGEX "/(gfx_gl|audio_openal)/tests/")

file(GLOB KISAK_ZLIB_C CONFIGURE_DEPENDS "${SRC_DIR}/zlib/*.c")
file(GLOB KISAK_JPEG_C CONFIGURE_DEPENDS "${SRC_DIR}/jpeg/*.c")
list(FILTER KISAK_JPEG_C EXCLUDE REGEX "/jmem(ansi|name)\\.c$")

set(KISAK_WMA_C)
foreach (name IN ITEMS wmadeci wmafixed mdct mdct_lookup fft-ffmpeg ffmpeg_bitstream wma_decode)
    set(source "${SRC_DIR}/audio_openal/libwma/${name}.c")
    if (EXISTS "${source}")
        list(APPEND KISAK_WMA_C "${source}")
    endif()
endforeach()

add_executable(${BIN_NAME}
    ${KISAK_PORTABLE_CPP}
    ${KISAK_ZLIB_C}
    ${KISAK_JPEG_C}
    ${KISAK_WMA_C}
)

target_include_directories(${BIN_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/platform/winsdk"
    "${SRC_DIR}/libs"
    "${SRC_DIR}/libs/libtomcrypt-1.17/src/headers"
    "${SRC_DIR}/libs/libtommath-1.0"
    "${SRC_DIR}/libs/libvpx-1.5.0/include"
    "${SRC_DIR}/audio_openal/libwma"
    "${CMAKE_CURRENT_SOURCE_DIR}/DemonWare"
    "${CMAKE_CURRENT_SOURCE_DIR}/tl"
    "${SRC_DIR}/jpeg"
)

target_compile_definitions(${BIN_NAME} PRIVATE KISAK_MP)
set_target_properties(${BIN_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    OUTPUT_NAME blackops
)

if (EMSCRIPTEN)
    option(KISAK_WEB_THREADS "Use Emscripten pthreads and Web Workers" OFF)
    option(KISAK_WEB_DEPROXY "Move WebGL rendering to the backend worker" OFF)
    option(KISAK_WEB_SAFE_HEAP "Enable Emscripten SAFE_HEAP diagnostics" OFF)
    option(KISAK_WEB_GL_DEBUG "Enable Emscripten WebGL diagnostics" OFF)

    if (KISAK_WEB_THREADS)
        set(KISAK_WEB_ASSERTIONS_DEFAULT OFF)
    else()
        set(KISAK_WEB_ASSERTIONS_DEFAULT ON)
    endif()
    option(KISAK_WEB_ASSERTIONS "Enable Emscripten runtime assertions" ${KISAK_WEB_ASSERTIONS_DEFAULT})

    target_compile_definitions(${BIN_NAME} PRIVATE KISAK_WEB)
    target_compile_options(${BIN_NAME} PRIVATE
        -fpermissive
        -w
        "$<$<COMPILE_LANGUAGE:CXX>:-msimd128;-msse;-msse2;-Wno-narrowing;-include;${SRC_DIR}/platform/compat/msvc_compat.h>"
        "$<$<COMPILE_LANGUAGE:C>:-D__cdecl=;-D__stdcall=;-D__fastcall=;-D__int8=char;-D__int16=short;-D__int32=int>"
        -sUSE_SDL=2
        -sMAX_WEBGL_VERSION=2
        -sMIN_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sALLOW_MEMORY_GROWTH=1
    )

    set(KISAK_WEB_LINK_OPTIONS
        -sUSE_SDL=2
        -sMAX_WEBGL_VERSION=2
        -sMIN_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=512MB
        -sMAXIMUM_MEMORY=2GB
        -sSTACK_SIZE=8MB
        -sALLOW_TABLE_GROWTH=1
        -sEMULATE_FUNCTION_POINTER_CASTS=1
        -sINVOKE_RUN=0
        -sERROR_ON_UNDEFINED_SYMBOLS=1
        -lopenal
        -O2
    )

    if (KISAK_WEB_ASSERTIONS)
        list(APPEND KISAK_WEB_LINK_OPTIONS -sASSERTIONS=1)
    else()
        list(APPEND KISAK_WEB_LINK_OPTIONS -sASSERTIONS=0)
    endif()

    if (KISAK_WEB_THREADS)
        target_compile_options(${BIN_NAME} PRIVATE -pthread -O2)
        list(APPEND KISAK_WEB_LINK_OPTIONS
            -pthread
            -sPROXY_TO_PTHREAD=1
            -sPTHREAD_POOL_SIZE=24
            -sALLOW_BLOCKING_ON_MAIN_THREAD=1
            -sOFFSCREEN_FRAMEBUFFER=1
            -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,stringToUTF8,stringToNewUTF8,UTF8ToString,lengthBytesUTF8,HEAPU8,HEAP32,HEAPF64
            -sEXPORTED_FUNCTIONS=_main,_malloc,_free
            --post-js
            "${SRC_DIR}/platform/web/kb_blit_patch.js"
            --profiling-funcs
        )
    else()
        target_compile_options(${BIN_NAME} PRIVATE -g -O0)
        list(APPEND KISAK_WEB_LINK_OPTIONS
            -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,stringToUTF8,UTF8ToString,lengthBytesUTF8,HEAPU8
            -sASYNCIFY=1
            -sASYNCIFY_STACK_SIZE=262144
        )
    endif()

    if (KISAK_WEB_DEPROXY)
        if (NOT KISAK_WEB_THREADS)
            message(FATAL_ERROR "KISAK_WEB_DEPROXY requires KISAK_WEB_THREADS")
        endif()
        target_compile_definitions(${BIN_NAME} PRIVATE KB_DEPROXY_BUILD)
        list(APPEND KISAK_WEB_LINK_OPTIONS
            -sOFFSCREENCANVAS_SUPPORT=1
            -sASYNCIFY=1
            -sASYNCIFY_IGNORE_INDIRECT=1
            "-sASYNCIFY_ADD=@${CMAKE_CURRENT_SOURCE_DIR}/asyncify_funcs.txt"
            -sASYNCIFY_IMPORTS=invoke_v,invoke_vi,invoke_ii,invoke_iii,invoke_vii
        )
    endif()
    if (KISAK_WEB_SAFE_HEAP)
        list(APPEND KISAK_WEB_LINK_OPTIONS -sSAFE_HEAP=2)
    endif()
    if (KISAK_WEB_GL_DEBUG)
        list(APPEND KISAK_WEB_LINK_OPTIONS -sGL_DEBUG=1)
    endif()

    target_link_options(${BIN_NAME} PRIVATE ${KISAK_WEB_LINK_OPTIONS})
    set_target_properties(${BIN_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
        SUFFIX ".js"
    )

    add_custom_command(TARGET ${BIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SRC_DIR}/platform/web/index.html"
            "$<TARGET_FILE_DIR:${BIN_NAME}>/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SRC_DIR}/platform/web/web_fs.js"
            "$<TARGET_FILE_DIR:${BIN_NAME}>/web_fs.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SRC_DIR}/platform/web/coi-serviceworker.js"
            "$<TARGET_FILE_DIR:${BIN_NAME}>/coi-serviceworker.js"
        VERBATIM
    )
    if (KISAK_WEB_THREADS)
        add_custom_command(TARGET ${BIN_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/serve_web_mt.py"
                "$<TARGET_FILE_DIR:${BIN_NAME}>/serve_web_mt.py"
            VERBATIM
        )
    endif()
else()
    target_compile_options(${BIN_NAME} PRIVATE
        -m32
        -fpermissive
        "$<$<COMPILE_LANGUAGE:CXX>:-malign-double;-mmmx;-msse;-msse2;-fms-extensions;-Wno-narrowing;-include;${SRC_DIR}/platform/compat/msvc_compat.h>"
        "$<$<COMPILE_LANGUAGE:C>:-D__cdecl=;-D__stdcall=;-D__fastcall=;-D__int8=char;-D__int16=short;-D__int32=int>"
        -w
    )
    target_link_options(${BIN_NAME} PRIVATE -m32 -rdynamic)
    target_link_directories(${BIN_NAME} PRIVATE /usr/lib/i386-linux-gnu)
    target_link_libraries(${BIN_NAME} PRIVATE
        SDL2
        GLEW
        GL
        openal
        speex
        vpx
        pthread
        m
        dl
    )
endif()
