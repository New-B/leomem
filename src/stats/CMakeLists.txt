add_library(leomem_stats STATIC
    counters.cc
)

target_include_directories(leomem_stats
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)