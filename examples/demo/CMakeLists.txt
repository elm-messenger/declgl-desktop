add_executable(declgl_demo
    main.cc
)

target_link_libraries(declgl_demo PRIVATE
    declgl
    declgl_proto
)
