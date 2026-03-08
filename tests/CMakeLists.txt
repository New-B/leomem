add_executable(leomem_test_smoke
    unit/test_smoke.cc
)

target_link_libraries(leomem_test_smoke
    PRIVATE LeoMem::leomem
)

add_test(NAME leomem_test_smoke COMMAND leomem_test_smoke)