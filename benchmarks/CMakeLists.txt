add_executable(leomem_micro_rw_latency
    micro/read_write_latency.cc
)

target_link_libraries(leomem_micro_rw_latency
    PRIVATE LeoMem::leomem
)

add_executable(leomem_micro_phase_shift
    micro/phase_shift_modes.cc
)

target_include_directories(leomem_micro_phase_shift
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_micro_phase_shift
    PRIVATE LeoMem::leomem
)

add_executable(leomem_micro_runtime_phase_shift
    micro/runtime_phase_shift.cc
)

target_include_directories(leomem_micro_runtime_phase_shift
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_micro_runtime_phase_shift
    PRIVATE LeoMem::leomem
)

add_executable(leomem_micro_adaptive_coherence_paper
    micro/adaptive_coherence_paper.cc
)

target_include_directories(leomem_micro_adaptive_coherence_paper
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_micro_adaptive_coherence_paper
    PRIVATE LeoMem::leomem
)

add_executable(leomem_workload_iterative_analytics
    workload/iterative_analytics.cc
)

target_include_directories(leomem_workload_iterative_analytics
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_workload_iterative_analytics
    PRIVATE LeoMem::leomem
)

add_executable(leomem_workload_ycsb_mixed
    workload/ycsb_mixed.cc
)

target_include_directories(leomem_workload_ycsb_mixed
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_workload_ycsb_mixed
    PRIVATE LeoMem::leomem
)

add_executable(leomem_workload_kmeans
    workload/kmeans_workload.cc
)

target_include_directories(leomem_workload_kmeans
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_workload_kmeans
    PRIVATE LeoMem::leomem
)

add_executable(leomem_workload_pagerank
    workload/pagerank_workload.cc
)

target_include_directories(leomem_workload_pagerank
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(leomem_workload_pagerank
    PRIVATE LeoMem::leomem
)
