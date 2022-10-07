if(WIN32)
	add_definitions(-D_BUILD_FOR_DLL_EXPORT)
	set(LOCAL_MODULE libLive555ClientSource)
else()
	set(LOCAL_MODULE Live555ClientSource)
endif()

set(BUILD_SHARED_LIBS ON)

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}
	${live555_SOURCE_DIR}/liveMedia/include
	${live555_SOURCE_DIR}/groupsock/include
	${live555_SOURCE_DIR}/UsageEnvironment/include
	${live555_SOURCE_DIR}/BasicUsageEnvironment/include
)

add_library(${LOCAL_MODULE}
    Live555ClientSource.cpp
)

target_link_libraries(${LOCAL_MODULE} PUBLIC
    live555_cxx_flags
    UsageEnvironment
    BasicUsageEnvironment
    liveMedia
    groupsock
)

if(LIVE555_EPOLL_SUPPORT)
    target_compile_definitions(${LOCAL_MODULE} PRIVATE READ_FROM_FILES_SYNCHRONOUSLY)
endif()

set_target_properties(${LOCAL_MODULE} PROPERTIES FOLDER "live555/lib")

install(TARGETS ${LOCAL_MODULE} EXPORT live555Target
    INCLUDES DESTINATION include/meere
    RUNTIME DESTINATION bin/$<CONFIG>
    LIBRARY DESTINATION lib/$<CONFIG>
    ARCHIVE DESTINATION lib/$<CONFIG>
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Live555ClientSource.h
    DESTINATION include/meere
)
