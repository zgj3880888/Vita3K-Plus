add_library(
	motion
	STATIC
	src/motion.cpp
	src/motion_input.cpp
)

target_include_directories(motion PUBLIC include)
target_link_libraries(motion PUBLIC emuenv SDL3::SDL3 util)
# config: the motion code checks whether the stick is standing in for the sensors, which is a
# setting, not a property of the pose it builds
target_link_libraries(motion PRIVATE ctrl config)
if(ANDROID)
    target_link_libraries(motion PRIVATE android)
endif()
