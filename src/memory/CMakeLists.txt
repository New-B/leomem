add_library(leomem_memory STATIC
    global/dsm_allocator.cc
    cache/cache_manager.cc
)

target_include_directories(leomem_memory
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_memory
    PRIVATE leomem_common
)