add_executable(leomem_micro_rw_latency
    micro/read_write_latency.cc
)

target_link_libraries(leomem_micro_rw_latency
    PRIVATE LeoMem::leomem
)