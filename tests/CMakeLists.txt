add_executable(glsl_es_translator_test
    glsl_es_translator_test.cc
    ${CMAKE_SOURCE_DIR}/src/renderer/programs/glsl_es_translator.cc
)

target_include_directories(glsl_es_translator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME glsl_es_translator COMMAND glsl_es_translator_test)

add_executable(image_decoder_test
    image_decoder_test.cc
    ${CMAKE_SOURCE_DIR}/src/resources/image_decoder.cc
)

target_include_directories(image_decoder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(image_decoder_test PRIVATE stb_image declgl_log)

add_test(NAME image_decoder COMMAND image_decoder_test)

if(BUILD_ELM_PLAYER)
    add_executable(elm_host_unbounded_execution_test
        elm_host_unbounded_execution_test.cc
    )

    target_link_libraries(elm_host_unbounded_execution_test PRIVATE declgl_elm_host)
    target_compile_definitions(elm_host_unbounded_execution_test PRIVATE
        UNBOUNDED_EXECUTION_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unbounded_execution.js"
    )

    add_test(NAME elm_host_unbounded_execution COMMAND elm_host_unbounded_execution_test)
endif()
