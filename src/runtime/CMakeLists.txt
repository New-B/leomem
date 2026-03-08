add_library(leomem_runtime STATIC
    context.cc
    init.cc
    config_loader.cc
)

target_include_directories(leomem_runtime
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_runtime
    PRIVATE leomem_common
)