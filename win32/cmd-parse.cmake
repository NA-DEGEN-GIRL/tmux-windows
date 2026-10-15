# Generate cmd-parse.c from cmd-parse.y with bison (win_bison from
# WinFlexBison works):
#
#   cmake -DBISON_EXECUTABLE=<bison> -DINPUT=<cmd-parse.y> -DOUTPUT=<cmd-parse.c>
#         [-DNO_LINES=ON] -P win32/cmd-parse.cmake
#
# The first line of the output records the SHA256 of the grammar it was
# generated from, so a build without bison can tell when the committed
# cmd-parse.c no longer matches cmd-parse.y. Line endings are normalized
# before hashing so a CRLF checkout gives the same hash.

foreach(var BISON_EXECUTABLE INPUT OUTPUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "cmd-parse.cmake: ${var} is not set")
    endif()
endforeach()

file(READ "${INPUT}" grammar)
string(REPLACE "\r\n" "\n" grammar "${grammar}")
string(SHA256 hash "${grammar}")

set(args)
if(NO_LINES)
    list(APPEND args -l)
endif()
execute_process(
    COMMAND "${BISON_EXECUTABLE}" ${args} -o "${OUTPUT}.tmp" "${INPUT}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(REMOVE "${OUTPUT}.tmp")
    message(FATAL_ERROR "${BISON_EXECUTABLE} failed: ${result}")
endif()

file(READ "${OUTPUT}.tmp" parser)
file(REMOVE "${OUTPUT}.tmp")
string(REPLACE "\r\n" "\n" parser "${parser}")
file(WRITE "${OUTPUT}" "/* cmd-parse.y SHA256: ${hash} */\n${parser}")
