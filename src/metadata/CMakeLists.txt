add_library(leomem_metadata STATIC
    block_table.cc
)

target_include_directories(leomem_metadata
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_metadata
    PRIVATE leomem_common
)