# hermes::gateway — Phase 11 gateway core: runner, session store, hooks,
# pairing, status/locks, and config.

include(FetchContent)

# ----- Dependencies ------------------------------------------------------

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "hermes::gateway — fetching nlohmann_json 3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Boost.Beast (WebSocket) — required for Discord gateway + Slack Socket Mode.
# Header-only; we pull in Boost::beast via find_package(Boost COMPONENTS system).
find_package(Boost QUIET COMPONENTS system)
if(Boost_FOUND)
    message(STATUS "hermes::gateway — boost found (${Boost_VERSION}); WebSocket realtime enabled")
else()
    message(STATUS "hermes::gateway — boost NOT found; WebSocket realtime will fall back to mock only")
endif()

find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    message(STATUS "hermes::gateway — OpenSSL NOT found; SHA256 hashing will fail at link time")
endif()

# ----- libopus (Phase 14 — Discord voice) --------------------------------

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS QUIET opus)
endif()
if(NOT OPUS_FOUND)
    # Fallback: direct link
    find_library(OPUS_LIBRARIES opus)
    find_path(OPUS_INCLUDE_DIRS opus/opus.h)
    if(OPUS_LIBRARIES AND OPUS_INCLUDE_DIRS)
        set(OPUS_FOUND TRUE)
    endif()
endif()

# ----- Library -----------------------------------------------------------

add_library(hermes_gateway STATIC
    src/gateway_config.cpp
    src/session_store.cpp
    src/hooks.cpp
    src/pairing.cpp
    src/status.cpp
    src/gateway_runner.cpp
    src/stream_consumer.cpp
    src/stream_consumer_text.cpp
    src/channel_directory.cpp
    src/message_mirror.cpp
    src/sticker_cache.cpp
    src/restart.cpp
    src/media_cache.cpp
    src/safe_fetch.cpp
    src/slack_subcommand_router.cpp
    src/channel_cache.cpp
    src/gateway_helpers.cpp
    src/session_manager.cpp
    src/message_pipeline.cpp
    src/backpressure.cpp
    src/metrics.cpp
    src/command_dispatcher.cpp
    src/shutdown_sequencer.cpp
    src/config_validator.cpp
    src/session_ttl.cpp
    src/base_depth.cpp
    src/api_server_depth.cpp
    platforms/base_adapter.cpp
    platforms/telegram.cpp
    platforms/websocket_transport.cpp
    platforms/discord.cpp
    platforms/discord_gateway.cpp
    platforms/opus_codec.cpp
    platforms/slack.cpp
    platforms/slack_socket_mode.cpp
    platforms/whatsapp.cpp
    platforms/signal.cpp
    platforms/matrix.cpp
    platforms/olm_session.cpp
    platforms/mattermost.cpp
    platforms/email.cpp
    platforms/sms.cpp
    platforms/dingtalk.cpp
    platforms/feishu.cpp
    platforms/wecom.cpp
    platforms/weixin.cpp
    platforms/bluebubbles.cpp
    platforms/home_assistant.cpp
    platforms/api_server.cpp
    platforms/webhook.cpp
    platforms/local.cpp
    platforms/adapter_factory.cpp
)

target_include_directories(hermes_gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_gateway PUBLIC cxx_std_17)

target_link_libraries(hermes_gateway
    PUBLIC
        hermes::core
        hermes::config
        hermes::llm
        hermes::agent
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes::state
        hermes_warnings
)

# OpenSSL::Crypto must be PUBLIC: its -isystem path affects which
# nlohmann_json headers the compiler finds (vcpkg vs system).  If this
# were PRIVATE, downstream consumers would compile against a different
# nlohmann ABI version and fail to link.
if(OpenSSL_FOUND)
    target_link_libraries(hermes_gateway PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(hermes_gateway PRIVATE HERMES_HAVE_OPENSSL=1)
endif()

if(Boost_FOUND)
    target_link_libraries(hermes_gateway PUBLIC Boost::boost Boost::system)
    target_compile_definitions(hermes_gateway PUBLIC HERMES_GATEWAY_HAS_BEAST=1)
endif()

if(OPUS_FOUND)
    target_compile_definitions(hermes_gateway PUBLIC HERMES_GATEWAY_HAS_OPUS)
    target_include_directories(hermes_gateway PRIVATE ${OPUS_INCLUDE_DIRS})
    target_link_libraries(hermes_gateway PRIVATE ${OPUS_LIBRARIES})
    message(STATUS "hermes/gateway: opus found — Discord voice enabled")
else()
    message(STATUS "hermes/gateway: opus NOT found — Discord voice disabled")
endif()

# ----- libolm (optional) — Matrix E2EE ----------------------------------
find_path(OLM_INCLUDE_DIR olm/olm.h)
find_library(OLM_LIBRARY olm)
if(OLM_INCLUDE_DIR AND OLM_LIBRARY)
    set(OLM_FOUND TRUE)
    target_compile_definitions(hermes_gateway PUBLIC HERMES_GATEWAY_HAS_OLM)
    target_include_directories(hermes_gateway PRIVATE ${OLM_INCLUDE_DIR})
    target_link_libraries(hermes_gateway PRIVATE ${OLM_LIBRARY})
    message(STATUS "hermes/gateway: olm found — Matrix E2EE enabled")
else()
    message(STATUS "hermes/gateway: olm NOT found — Matrix E2EE disabled (install libolm-dev to enable)")
endif()

add_library(hermes::gateway ALIAS hermes_gateway)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
