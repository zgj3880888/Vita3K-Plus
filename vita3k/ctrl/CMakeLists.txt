add_library(
	ctrl
	STATIC
	include/ctrl/ctrl.h
	include/ctrl/functions.h
	include/ctrl/state.h
	src/ctrl.cpp
)

target_include_directories(ctrl PUBLIC include)
target_link_libraries(ctrl PUBLIC emuenv SDL3::SDL3 util)
# touch: ctrl reads L3/R3 off the pad and holds the touch mapping points down with them. The two
# libraries already refer to each other (touch calls back into ctrl), which is fine for static libs.
target_link_libraries(ctrl PRIVATE config display kernel touch)
if(ANDROID)
	target_link_libraries(ctrl PRIVATE android)
endif()
