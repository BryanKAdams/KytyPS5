set(KYTY_GIT_VERSION "unknown")
set(KYTY_GIT_HASH "unknown")
set(KYTY_GIT_REVISION "unknown")
if(GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
		WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
		OUTPUT_VARIABLE KYTY_GIT_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GIT_RESULT
		ERROR_QUIET
	)
	if(NOT GIT_RESULT EQUAL 0)
		set(KYTY_GIT_VERSION "unknown")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
		WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
		OUTPUT_VARIABLE KYTY_GIT_REVISION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GIT_HASH_RESULT
		ERROR_QUIET
	)
	if(NOT GIT_HASH_RESULT EQUAL 0)
		set(KYTY_GIT_HASH "unknown")
		set(KYTY_GIT_REVISION "unknown")
	else()
		string(SUBSTRING "${KYTY_GIT_REVISION}" 0 7 KYTY_GIT_HASH)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" diff-index --quiet HEAD --
			WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
			RESULT_VARIABLE GIT_DIRTY_RESULT
			ERROR_QUIET
		)
		if(NOT GIT_DIRTY_RESULT EQUAL 0)
			string(APPEND KYTY_GIT_HASH "-dirty")
		endif()
	endif()
endif()

# Recorded shaders and driver pipelines depend only on the recompiler and pipeline code. Hashing
# those sources (including uncommitted edits) keeps a title's caches valid across unrelated
# commits, and invalidates them for any change that can alter compilation or journal records.
set(KYTY_SHADER_CACHE_KEY "unknown")
if(SOURCE_DIRECTORY)
	set(shader_cache_files)
	foreach(directory graphics/shader graphics/host_gpu/renderer/pipeline)
		file(GLOB_RECURSE directory_files LIST_DIRECTORIES false
			"${SOURCE_DIRECTORY}/${directory}/*")
		list(APPEND shader_cache_files ${directory_files})
	endforeach()
	list(SORT shader_cache_files)
	set(shader_cache_digest "")
	foreach(source_file ${shader_cache_files})
		file(SHA256 "${source_file}" source_hash)
		file(RELATIVE_PATH source_name "${SOURCE_DIRECTORY}" "${source_file}")
		string(APPEND shader_cache_digest "${source_name}:${source_hash}\n")
	endforeach()
	if(shader_cache_files)
		string(SHA256 KYTY_SHADER_CACHE_KEY "${shader_cache_digest}")
	endif()
endif()

configure_file("${INPUT_FILE}" "${OUTPUT_FILE}")
