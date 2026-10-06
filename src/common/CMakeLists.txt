add_library(rsba_common STATIC
    audio_codec.c
    audio_dev.c
    audio_dev_alsa.c
    audio_dev_winmm.c
    audio_link.c
    cfg_load.c
    civ.c
    civ_link.c
    ctl_proto.c
    dch.c
    jbuf.c
    json.c
    net_socket.c
    rs_crypto.c
    rs_error.c
    rs_log.c
    rs_shutdown.c
    rs_time.c
    serial_port.c)

target_include_directories(rsba_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rsba_common PUBLIC rsba_build_options)

if(WIN32)
    target_link_libraries(rsba_common PUBLIC ws2_32 bcrypt winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(rsba_common PUBLIC Threads::Threads)

    # ALSA は任意。見つからなければ "alsa" バックエンドは RS_ERR_UNSUPPORTED を返す
    if(RSBA_REQUIRE_ALSA)
        find_package(ALSA REQUIRED) # 実機（Raspberry Pi）向けビルドで音声なしバイナリを作らない
    else()
        find_package(ALSA)
    endif()
    if(ALSA_FOUND)
        target_compile_definitions(rsba_common PRIVATE RSBA_HAVE_ALSA)
        target_link_libraries(rsba_common PUBLIC ALSA::ALSA)
    else()
        message(STATUS "ALSA not found: audio backend 'alsa' disabled (install libasound2-dev)")
    endif()
endif()
