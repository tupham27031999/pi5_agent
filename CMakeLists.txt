cmake_minimum_required(VERSION 3.16)
project(pi5_agent_server LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    include
)

set(AGENT_SOURCES
    src/main.cpp
    src/http_server.cpp
    src/process_runner.cpp
    src/zip_unpacker.cpp
)

add_executable(pi5_agent_server ${AGENT_SOURCES})

if(WIN32)
    target_link_libraries(pi5_agent_server PRIVATE ws2_32 advapi32)
elseif(UNIX)
    target_link_libraries(pi5_agent_server PRIVATE pthread)
endif()
