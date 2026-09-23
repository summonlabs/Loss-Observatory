add_executable(lo_bench lo_bench.cpp)
target_link_libraries(lo_bench PRIVATE LossObservatory::loss_observatory LossObservatory::Warnings)
