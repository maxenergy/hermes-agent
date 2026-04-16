# hermes::mcp_server — native MCP (Model Context Protocol) server.
#
# Provides two surfaces:
#   * ``HermesMcpServer`` — legacy stdio JSON-RPC server (unchanged).
#   * ``McpServer``        — HTTP/1.1 + SSE transport with session GC,
#                             tool-registry forwarding, and resource /
#                             prompt providers.

find_package(Boost QUIET COMPONENTS system)

add_library(hermes_mcp_server STATIC
    src/mcp_server.cpp
    src/rpc_types.cpp
    src/session.cpp
    src/rpc_dispatch.cpp
    src/http_server.cpp
)

target_include_directories(hermes_mcp_server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_mcp_server PUBLIC cxx_std_17)

# ``hermes::tools`` must be PUBLIC because ``rpc_dispatch.hpp`` forward-
# declares ``ToolRegistry`` but source consumers that wire the server up
# need the full definition to hand in a registry pointer.
target_link_libraries(hermes_mcp_server
    PUBLIC
        hermes::state
        hermes::core
        hermes::tools
        hermes::llm
    PRIVATE
        hermes_warnings
)

if(Boost_FOUND)
    target_link_libraries(hermes_mcp_server PUBLIC Boost::boost Boost::system)
else()
    message(FATAL_ERROR
        "hermes::mcp_server requires Boost (asio + beast). Install "
        "libboost-all-dev (Debian/Ubuntu) or equivalent.")
endif()

add_library(hermes::mcp_server ALIAS hermes_mcp_server)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
