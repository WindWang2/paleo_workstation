# Build step. Immutable generations survive clean, deleted generations are rebuilt.
cmake_minimum_required(VERSION 3.28)
if(EXISTS "${PI_STORE}/generations/${PI_SHA}/pi-manifest.json")
  return()
endif()
set(tools "${CMAKE_CURRENT_LIST_DIR}/../tools")
execute_process(COMMAND "${PI_PYTHON}" -B "${tools}/fetch_pi_archives.py" --cache "${PI_CACHE}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PI_PYTHON}" -B "${tools}/prepare_pi_agent.py" --offline
  --cache "${PI_CACHE}" --store "${PI_STORE}" --platform "${PI_PLATFORM}" COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${PI_STORE}/generations/${PI_SHA}/pi-manifest.json")
  message(FATAL_ERROR "Prepared Pi runtime does not match the repository pin ${PI_SHA}")
endif()
