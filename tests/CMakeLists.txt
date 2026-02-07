include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(lsm_add_test name)
  add_executable(${name} ${name}.cc)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE lsm lsm_warnings GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

lsm_add_test(arena_test)
lsm_add_test(db_api_test)
lsm_add_test(db_test)
lsm_add_test(file_test)
lsm_add_test(log_test)
lsm_add_test(memtable_test)
lsm_add_test(status_test)
lsm_add_test(coding_test)
lsm_add_test(crc32c_test)
lsm_add_test(dbformat_test)
lsm_add_test(write_batch_test)
