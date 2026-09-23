add_library(lo_test_framework STATIC
  framework/lo_test.cpp
)
target_include_directories(lo_test_framework PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/framework")
target_link_libraries(lo_test_framework PRIVATE LossObservatory::Warnings)

# Shared fixtures for the suites.
add_library(lo_test_fixtures STATIC
  framework/fixtures.cpp
)
target_include_directories(lo_test_fixtures PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/framework")
target_compile_definitions(lo_test_fixtures PUBLIC
  LO_TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/data"
)
target_link_libraries(lo_test_fixtures
  PUBLIC LossObservatory::loss_observatory lo_test_framework
  PRIVATE LossObservatory::Warnings
)

set(LO_UNIT_SOURCES
  unit/test_core.cpp
  unit/test_model.cpp
  unit/test_counter.cpp
  unit/test_freshness.cpp
  unit/test_classify.cpp
  unit/test_conflict.cpp
  unit/test_localize.cpp
  unit/test_episode.cpp
  unit/test_aggregate.cpp
  unit/test_persist.cpp
  unit/test_script.cpp
)

set(LO_INTEGRATION_SOURCES
  integration/test_engine.cpp
  integration/test_restart.cpp
)

set(LO_INVARIANTS_SOURCES
  invariants/test_properties.cpp
  invariants/test_adversarial.cpp
  invariants/test_hardening.cpp
)

set(LO_CONCURRENCY_SOURCES
  concurrency/test_concurrency.cpp
)

set(LO_E2E_SOURCES
  e2e/test_cli_process.cpp
)

function(lo_add_suite name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lo_test_fixtures LossObservatory::loss_observatory)
  target_compile_definitions(${name} PRIVATE
    LO_LOCCTL_PATH="$<TARGET_FILE:locctl>"
  )
  add_dependencies(${name} locctl)
endfunction()

lo_add_suite(lo_tests_unit ${LO_UNIT_SOURCES})
lo_add_suite(lo_tests_integration ${LO_INTEGRATION_SOURCES})
lo_add_suite(lo_tests_invariants ${LO_INVARIANTS_SOURCES})
lo_add_suite(lo_tests_concurrency ${LO_CONCURRENCY_SOURCES})
lo_add_suite(lo_tests_e2e ${LO_E2E_SOURCES})

# Whole-suite entries.
add_test(NAME suite.unit COMMAND lo_tests_unit)
add_test(NAME suite.integration COMMAND lo_tests_integration)
add_test(NAME suite.invariants COMMAND lo_tests_invariants)
add_test(NAME suite.concurrency COMMAND lo_tests_concurrency)
add_test(NAME suite.e2e COMMAND lo_tests_e2e)

# Named proof entries. Each one names the property it demonstrates, so a
# failure report says which guarantee broke rather than just which file failed.
add_test(NAME proof.stale_evidence_never_proves_current_loss
  COMMAND lo_tests_unit --filter=classification.stale_evidence_is_never_current_loss)
add_test(NAME proof.discontinuities_are_explicit
  COMMAND lo_tests_unit --filter=counter.reset_is_a_discontinuity_not_loss)
add_test(NAME proof.disagreement_yields_conflict
  COMMAND lo_tests_unit --filter=conflict.unresolved_disagreement_becomes_conflicting_evidence)
add_test(NAME proof.localization_never_exceeds_evidence_granularity
  COMMAND lo_tests_unit --filter=localize.flow_scoped_evidence_never_names_a_hop)
add_test(NAME proof.restart_does_not_revive_old_loss
  COMMAND lo_tests_integration --filter=restart.open_episode_is_sealed_and_not_revived)
add_test(NAME proof.determinism_under_concurrent_ingest
  COMMAND lo_tests_concurrency --filter=concurrency.shuffled_arrival_is_equivalent)
add_test(NAME proof.bounded_aggregation_reports_truncation
  COMMAND lo_tests_unit --filter=aggregate.window_bound_is_reported)
add_test(NAME proof.persistence_integrity_check
  COMMAND lo_tests_unit --filter=persist.corrupted_record_stops_recovery_conservatively)
add_test(NAME proof.independent_process_round_trip
  COMMAND lo_tests_e2e --filter=e2e.independent_process_export_and_reingest)
add_test(NAME proof.absence_of_evidence_is_not_loss
  COMMAND lo_tests_unit --filter=classification.absent_evidence_is_not_loss)
