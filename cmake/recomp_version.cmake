# Writes OUT: WWHD_RECOMP_VERSION, a hash of the files that determine the code the on-device
# recompiler produces (FILES, "|"-separated) and of EXTRA (the LLVM version). The code cache is
# compiled again only when this (or the game executable, or the CPU) changes.
string(REPLACE "|" ";" files "${FILES}")
set(all "${EXTRA}")
foreach(f ${files})
  file(SHA256 ${f} h)
  string(APPEND all "${h}")
endforeach()
string(SHA256 sum "${all}")
file(WRITE ${OUT}.tmp "#define WWHD_RECOMP_VERSION \"${sum}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.tmp ${OUT})
