add_subdirectory(common)
add_subdirectory(runtime)
add_subdirectory(transport)
add_subdirectory(memory)
add_subdirectory(metadata)
add_subdirectory(stats)

add_library(leomem STATIC
    api/alloc_api.cc
    api/access_api.cc
    api/sync_api.cc
    api/admin_api.cc
)

add_library(LeoMem::leomem ALIAS leomem)

target_include_directories(leomem
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem
    PUBLIC
        Threads::Threads
    PRIVATE
        leomem_common
        leomem_runtime
        leomem_transport
        leomem_memory
        leomem_metadata
        leomem_stats
)