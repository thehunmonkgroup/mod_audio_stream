cmake_minimum_required(VERSION 3.18)
project(mod_audio_stream
        VERSION 1.0.0
        DESCRIPTION "Audio streaming module for FreeSWITCH."
        HOMEPAGE_URL "https://github.com/amigniter/mod_audio_stream")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

option(ENABLE_LOCAL "Enable local compile/debug specific" OFF)
if(ENABLE_LOCAL)
    set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

option(MOD_AUDIO_STREAM_DEB_LOCAL_FREESWITCH
    "Build DEB for source-built/local FreeSWITCH instead of distro-packaged FreeSWITCH"
    OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
find_package(SpeexDSP REQUIRED)

pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)

if(MOD_AUDIO_STREAM_DEB_LOCAL_FREESWITCH)
    set(_mod_audio_stream_default_module_install_dir "/usr/lib/mod-audio-stream")
else()
    set(_mod_audio_stream_default_module_install_dir "${FS_MOD_DIR}")
endif()

set(MOD_AUDIO_STREAM_MODULE_INSTALL_DIR
    "${_mod_audio_stream_default_module_install_dir}"
    CACHE PATH "Install directory for mod_audio_stream.so")

message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")
message(STATUS "mod_audio_stream install dir: ${MOD_AUDIO_STREAM_MODULE_INSTALL_DIR}")

add_subdirectory(libs/libwsc)

add_library(mod_audio_stream SHARED 
    mod_audio_stream.c
    mod_audio_stream.h
    audio_streamer_glue.h
    audio_streamer_glue.cpp
    inbound_playback.h
    inbound_playback.cpp
    base64.cpp
)

set_property(TARGET mod_audio_stream PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(mod_audio_stream PRIVATE 
    PkgConfig::FreeSWITCH 
    pthread
    libwsc
)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set_target_properties(${PROJECT_NAME} 
        PROPERTIES 
        LINK_FLAGS_RELEASE "-s")
endif()

set(CPACK_COMPONENTS_ALL ${PROJECT_NAME} changelog.gz copyright)

if(MOD_AUDIO_STREAM_DEB_LOCAL_FREESWITCH)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    # Local FreeSWITCH installs do not map libfreeswitch.so.1 to a Debian package.
    set(_deb_depends
        libc6
        libstdc++6
        libspeexdsp1
        zlib1g
        libevent-2.1-7
        libevent-pthreads-2.1-7
    )

    if(USE_TLS)
        list(APPEND _deb_depends
            libevent-openssl-2.1-7
            libssl3
        )
    endif()

    list(JOIN _deb_depends ", " CPACK_DEBIAN_PACKAGE_DEPENDS)
else()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    unset(CPACK_DEBIAN_PACKAGE_DEPENDS)
endif()

set(CPACK_PACKAGE_NAME "mod-audio-stream")
set(CMAKE_INSTALL_DOCDIR "share/doc/${CPACK_PACKAGE_NAME}")

add_custom_command(
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog"
    COMMAND gzip -cn9 "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" > "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
    COMMENT "Processing copyright file"
)
add_custom_target(changelog_target ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz")

add_custom_command(
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debian/copyright"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/debian/copyright" "${CMAKE_CURRENT_BINARY_DIR}/copyright"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/copyright"
    COMMENT "Generating compressed changelog.gz"
)
add_custom_target(copyright_target ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/copyright")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/copyright"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        COMPONENT copyright
)

install(TARGETS ${PROJECT_NAME}
        COMPONENT ${PROJECT_NAME}
        DESTINATION "${MOD_AUDIO_STREAM_MODULE_INSTALL_DIR}")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
	DESTINATION "${CMAKE_INSTALL_DOCDIR}"
	COMPONENT changelog.gz
)

message(STATUS "Components to pack: ${CPACK_COMPONENTS_ALL}")

include(Packing)
