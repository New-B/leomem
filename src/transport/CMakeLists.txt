add_library(leomem_transport STATIC
    transport_stub.cc
)

target_include_directories(leomem_transport
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_transport
    PRIVATE leomem_common
)