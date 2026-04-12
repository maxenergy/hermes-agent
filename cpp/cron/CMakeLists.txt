# hermes::cron — Phase 10 cron scheduler, expression parser, and delivery router.

add_library(hermes_cron STATIC
    src/cron_parser.cpp
    src/jobs.cpp
    src/scheduler.cpp
    src/delivery.cpp
)

target_include_directories(hermes_cron
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_cron PUBLIC cxx_std_17)

target_link_libraries(hermes_cron
    PUBLIC
        hermes::core
    PRIVATE
        hermes_warnings
)

add_library(hermes::cron ALIAS hermes_cron)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
