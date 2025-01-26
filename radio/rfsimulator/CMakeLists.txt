if(OAI_ZMQ)
    find_library(ZeroMQ zmq REQUIRED)
    add_library(rfsimulator MODULE
    simulator_pub_sub.c
    apply_channelmod.c
    ../../openair1/PHY/TOOLS/signal_energy.c
    )
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(ZMQ REQUIRED libzmq)

    include_directories(${ZMQ_INCLUDE_DIRS})

    set_target_properties(rfsimulator PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    add_executable(replay_node stored_node.c)

    target_link_libraries (replay_node minimal_lib ${ZMQ_LIBRARIES})

    target_link_libraries(rfsimulator PRIVATE SIMU ${ZMQ_LIBRARIES})

    add_executable(broker broker.c)
    target_link_libraries(broker PRIVATE ${ZMQ_LIBRARIES})
    set_target_properties(broker PROPERTIES  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
else()
    add_library(rfsimulator MODULE
                simulator.c
                apply_channelmod.c
                ../../openair1/PHY/TOOLS/signal_energy.c
    )
    target_link_libraries(rfsimulator PRIVATE SIMU HASHTABLE)
    set_target_properties(rfsimulator PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_executable(replay_node stored_node.c)
    target_link_libraries (replay_node minimal_lib)
endif()
