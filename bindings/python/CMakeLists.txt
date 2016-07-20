find_package(Boost REQUIRED COMPONENTS python)
find_package(PythonLibs REQUIRED)
find_package(PythonInterp REQUIRED)
find_package(PythonExtensionDir REQUIRED)
#message(STATUS "Python includes are situated in (${PYTHON_INCLUDE_PATH}, ${PYTHON_INCLUDE_DIRS})")

include_directories(BEFORE ${PROJECT_SOURCE_DIR}/include)
include_directories(${Boost_INCLUDE_DIRS})
link_directories(${Boost_LIBRARY_DIRS})

# find_package(PythonLibs REQUIRED)
include_directories(${PYTHON_INCLUDE_PATH})
# include_directories(${PYTHON_INCLUDE_DIRS})
# link_directories(${PYTHON_LIBRARIES})

add_library(mastermind-cache-python-extension SHARED main.cpp)
target_link_libraries(mastermind-cache-python-extension
    ${Boost_LIBRARIES}
    ${PYTHON_LIBRARIES}
    mastermind-cache
)

set_target_properties(mastermind-cache-python-extension PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mastermind_cache"
    #TODO: describe
    # SOVERSION ${MAJOR_VERSION}
    )

configure_file(setup.py.in ${CMAKE_CURRENT_BINARY_DIR}/setup.py)

add_custom_command(TARGET mastermind-cache-python-extension POST_BUILD
    COMMENT "-- Building wheel with python extension in ${CMAKE_CURRENT_BINARY_DIR}/dist"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND ${PYTHON_EXECUTABLE} ./setup.py bdist_wheel
)

install(CODE "
    file(GLOB WHEEL RELATIVE ${CMAKE_CURRENT_BINARY_DIR}/dist ${CMAKE_CURRENT_BINARY_DIR}/dist/*.whl)
    set(_INSTALL_DIR \$ENV{DESTDIR}${PYTHON_EXTENSION_DIR})
    message(\"-- Installing python extension files:\")
    message(\"-- From wheel: '\${WHEEL}'\")
    message(\"-- To dir: '\${_INSTALL_DIR}'\")
    execute_process(
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND ${PYTHON_EXECUTABLE} -mpip install -v --isolated --no-deps --no-index --ignore-installed --target=\${_INSTALL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dist/\${WHEEL}
        RESULT_VARIABLE _INSTALL_RESULT
    )
    if (_INSTALL_RESULT)
        message(FATAL_ERROR \"Error installing python extension files from the wheel\")
    endif()
    unset(_INSTALL_DIR)
    unset(_INSTALL_RESULT)
")
