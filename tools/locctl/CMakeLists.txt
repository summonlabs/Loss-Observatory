add_executable(locctl main.cpp)
target_link_libraries(locctl PRIVATE LossObservatory::loss_observatory LossObservatory::Warnings)

install(TARGETS locctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
