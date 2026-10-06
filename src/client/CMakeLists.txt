add_library(rsba_client STATIC
    client_app.c
    client_config.c
    ctl_client.c)

target_include_directories(rsba_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rsba_client PUBLIC rsba_common)

add_executable(client_bridge main.c)
target_link_libraries(client_bridge PRIVATE rsba_client)
