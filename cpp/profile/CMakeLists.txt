# hermes::profile — HERMES_HOME override + profile CRUD.

add_library(hermes_profile STATIC
    src/profile.cpp
)

target_include_directories(hermes_profile
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_profile PUBLIC cxx_std_17)

target_link_libraries(hermes_profile
    PUBLIC
        hermes::core
    PRIVATE
        hermes_warnings
)

add_library(hermes::profile ALIAS hermes_profile)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
