function(run_console name commands)
    set(input "${CMAKE_CURRENT_BINARY_DIR}/${name}.stdin")
    file(WRITE "${input}" "${commands}")
    execute_process(
        COMMAND "${OPENT1_EXECUTABLE}" --headless --no-audio --no-assets -dedicated ${ARGN}
        INPUT_FILE "${input}"
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE result
        TIMEOUT 10
    )
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${name} failed (${result}):\n${output}\n${errors}")
    endif()
    if(NOT output MATCHES "OpenT1 script console ready")
        message(FATAL_ERROR "${name} did not publish its ready message on stdout: ${output}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
endfunction()

run_console(commands "echo(\"STDIN_LOG\");\r\nreturn 6 * 7;\necho(;\necho(\"AFTER_ERROR\");\nquit();\necho(\"AFTER_QUIT\");\n")
if(NOT commands_output MATCHES "STDIN_LOG" OR NOT commands_output MATCHES "42" OR
   NOT commands_output MATCHES "<stdin>" OR NOT commands_output MATCHES "AFTER_ERROR" OR
   commands_output MATCHES "AFTER_QUIT")
    message(FATAL_ERROR "Command execution, diagnostics, or quit handling failed: ${commands_output}")
endif()

run_console(partial "echo(\"FINAL_PARTIAL\"); quit();")
if(NOT partial_output MATCHES "FINAL_PARTIAL")
    message(FATAL_ERROR "Final unterminated stdin line was lost: ${partial_output}")
endif()

# An idle/closed input must not hold up a bounded run or its shutdown.
run_console(empty "" --frames 4)

# Reject the whole overlong line; never execute a silently truncated prefix.
string(REPEAT " " 65536 padding)
run_console(overlong "echo(\"TRUNCATED_PREFIX\");${padding}\necho(\"RECOVERED_LONG_LINE\"); quit();\n")
if(NOT overlong_output MATCHES "discarded command" OR
   NOT overlong_output MATCHES "RECOVERED_LONG_LINE" OR overlong_output MATCHES "TRUNCATED_PREFIX")
    message(FATAL_ERROR "Overlong command recovery failed: ${overlong_output}")
endif()
