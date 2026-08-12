add_executable(basic-example basic-example.cpp)
add_executable(summarizer-example summarizer-example.cpp)
add_executable(custom-policy-example custom-policy-example.cpp)

target_link_libraries(basic-example PRIVATE Alcami::alcami)
target_link_libraries(summarizer-example PRIVATE Alcami::alcami)
target_link_libraries(custom-policy-example PRIVATE Alcami::alcami)
