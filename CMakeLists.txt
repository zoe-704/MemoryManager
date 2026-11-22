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
  "vm/vm.c" "init/init.c" "globals/globals.c" "pte/pte.c" "fault/fault.c" "pfn/pfn.c" "list/list.c" "stats/stats.c" "disc/disc.c"
  "vm/vm.h" "init/init.h" "globals/globals.h" "pte/pte.h" "fault/fault.h" "pfn/pfn.h" "list/list.h" "stats/stats.h" "disc/disc.h")

# Each module includes sibling headers by bare name, so put every module
# directory on the include path.
target_include_directories (ZoeVM PRIVATE
  "vm" "init" "globals" "pte" "fault" "pfn" "list" "stats" "disc")

# TODO: Add tests and install targets if needed.
