add_library(rsba_server STATIC
    ban_list.c
    ctl_server.c
    server_app.c
    server_config.c
    usb_device.c
    usb_monitor.c)

if(WIN32)
    target_sources(rsba_server PRIVATE usb_backend_win.c)
    target_link_libraries(rsba_server PUBLIC cfgmgr32)
else()
    target_sources(rsba_server PRIVATE usb_backend_linux.c)
endif()

target_include_directories(rsba_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rsba_server PUBLIC rsba_common)

add_executable(server_bridge main.c)
target_link_libraries(server_bridge PRIVATE rsba_server)
