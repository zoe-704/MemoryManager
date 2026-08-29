# CMakeList.txt : Top-level CMake project file, do global configuration
# and define the ZoeVM executable here.
#
cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("ZoeVM")

# Add source to this project's executable.
add_executable (ZoeVM
  "vm.c" "init.c" "globals.c" "pte.c" "fault.c" "pfn.c" "list.c" "stats.c" "disc.c"
  "vm.h" "init.h" "globals.h" "pte.h" "fault.h" "pfn.h" "list.h" "stats.h" "disc.h")

# TODO: Add tests and install targets if needed.
