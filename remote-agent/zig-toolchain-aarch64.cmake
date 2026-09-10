set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_CXX_COMPILER "zig" "c++" "-target" "aarch64-linux-musl")
set(CMAKE_C_COMPILER "zig" "cc" "-target" "aarch64-linux-musl")
set(CMAKE_AR "${ZIG_PATH}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${ZIG_PATH}" CACHE FILEPATH "" FORCE)
