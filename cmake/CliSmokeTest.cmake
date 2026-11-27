# Runs scantailor-cli on one small text page; ctest sets a platform plugin that does not exist, so the run proves that the CLI needs none.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

execute_process(COMMAND "${CLI}" -o "${WORK}/out" "${PAGE}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if (NOT rc EQUAL 0)
  message(FATAL_ERROR "scantailor-cli exit status ${rc}\n${out}\n${err}")
endif()
foreach (result "out/cli_smoke_page.tif" "out/project.ScanTailor")
  if (NOT EXISTS "${WORK}/${result}")
    message(FATAL_ERROR "scantailor-cli did not write ${result}\n${out}\n${err}")
  endif()
endforeach()

execute_process(COMMAND "${CLI}" --no-such-option RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if (NOT rc EQUAL 2)
  message(FATAL_ERROR "a bad option gave exit status ${rc}, not 2")
endif()
