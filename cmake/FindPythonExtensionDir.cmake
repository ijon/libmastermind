execute_process(COMMAND "${PYTHON_EXECUTABLE}" -c "from distutils import sysconfig; print sysconfig.get_python_lib(prefix='${CMAKE_INSTALL_PREFIX}')"
	RESULT_VARIABLE _FAILED
	OUTPUT_VARIABLE PYTHON_EXTENSION_DIR
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (_FAILED)
    message(FATAL_ERROR "Can't python packages directory")
endif()

#TODO: describe the reason
string(REGEX REPLACE "site-packages$" "dist-packages" PYTHON_EXTENSION_DIR "${PYTHON_EXTENSION_DIR}")

unset(_FAILED)
