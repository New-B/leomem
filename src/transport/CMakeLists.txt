add_library(leomem_transport STATIC
    transport_stub.cc
    rdma_transport.cc
)

target_include_directories(leomem_transport
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

if(LEOMEM_ENABLE_RDMA)
    find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
    find_library(IBVERBS_LIBRARY ibverbs)

    if(NOT IBVERBS_INCLUDE_DIR OR NOT IBVERBS_LIBRARY)
        message(FATAL_ERROR "LEOMEM_ENABLE_RDMA=ON but libibverbs was not found")
    endif()

    target_include_directories(leomem_transport PRIVATE ${IBVERBS_INCLUDE_DIR})
    target_link_libraries(leomem_transport PRIVATE ${IBVERBS_LIBRARY})
    target_compile_definitions(leomem_transport PRIVATE LEOMEM_HAS_IBVERBS=1)
endif()

target_link_libraries(leomem_transport
    PRIVATE leomem_common
)
