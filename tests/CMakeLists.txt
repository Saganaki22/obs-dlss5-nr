add_executable(frame-policy-test frame-policy-test.cpp)
add_test(NAME frame-policy COMMAND frame-policy-test)

add_executable(ngx-diagnostics-test ngx-diagnostics-test.cpp)
add_test(NAME ngx-diagnostics COMMAND ngx-diagnostics-test)

if(WIN32)
  add_executable(ngx-log-lifetime-test ngx-log-lifetime-test.cpp)
  add_test(NAME ngx-log-lifetime COMMAND ngx-log-lifetime-test $<TARGET_FILE:nr-caller-shim>)
endif()

if(WIN32 AND ENABLE_GPU_SMOKE_TEST)
  add_executable(obs-performance-smoke obs-performance-smoke.cpp)
  target_link_libraries(obs-performance-smoke PRIVATE OBS::libobs)
  add_library(fake-nr-runtime SHARED fake-nr-runtime.cpp)
  target_link_libraries(fake-nr-runtime PRIVATE OBS::libobs)
  add_library(fake-ngx-core SHARED fake-ngx-core.cpp)
endif()
