# CMakeList.txt : CMake project for ZoeVM, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (ZoeVM "vm.c" "init.c" "globals.c" "vm.h" "pte.c" "fault.c" "pfn.c" "list.c")

# TODO: Add tests and install targets if needed.

