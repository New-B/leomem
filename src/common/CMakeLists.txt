add_library(leomem_common STATIC
    log.cc
    utils.cc
)

target_include_directories(leomem_common
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)