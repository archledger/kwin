include_directories(${Libinput_INCLUDE_DIRS})

add_library(LibInputTestObjects STATIC ../../src/backends/libinput/device.cpp ../../src/backends/libinput/events.cpp ../../src/core/inputdevice.cpp ../../src/mousebuttons.cpp mock_libinput.cpp)
target_compile_definitions(LibInputTestObjects PRIVATE KWIN_BUILD_TESTING)
target_link_libraries(LibInputTestObjects Qt::Test Qt::Quick Qt::Widgets Qt::DBus Qt::Gui KF6::ConfigCore)
target_include_directories(LibInputTestObjects PUBLIC ${CMAKE_SOURCE_DIR}/src)

########################################################
# Test Connection Startup
########################################################
add_executable(testLibinputConnection
    connection_test.cpp
    ../../src/backends/libinput/connection.cpp
    ../../src/backends/libinput/context.cpp
    ../../src/backends/libinput/libinputbackend.cpp
    ../../src/backends/libinput/libinput_logging.cpp
)
# Compile the production connection/context, with the mock transport and Device
# objects taking precedence over libkwin. libkwin supplies Workspace, Udev,
# Session's meta-object, gainRealTime and the remaining backend infrastructure.
# Only LibInputTestObjects excludes Device's output mapping for these tests.
target_link_libraries(testLibinputConnection
    LibInputTestObjects
    kwin
    Libinput::Libinput
    Qt::Test
    KF6::ConfigCore
)
add_test(NAME kwin-testLibinputConnection COMMAND dbus-run-session ${CMAKE_BINARY_DIR}/bin/testLibinputConnection)
set_tests_properties(kwin-testLibinputConnection PROPERTIES TIMEOUT 30)
ecm_mark_as_test(testLibinputConnection)

########################################################
# Test Devices
########################################################
add_executable(testLibinputDevice device_test.cpp)
target_link_libraries(testLibinputDevice Qt::Test Qt::DBus Qt::Gui KF6::ConfigCore LibInputTestObjects)
add_test(NAME kwin-testLibinputDevice COMMAND testLibinputDevice)
ecm_mark_as_test(testLibinputDevice)

########################################################
# Test Key Event
########################################################
add_executable(testLibinputKeyEvent key_event_test.cpp)
target_link_libraries(testLibinputKeyEvent Qt::Test Qt::DBus Qt::Widgets KF6::ConfigCore LibInputTestObjects)
add_test(NAME kwin-testLibinputKeyEvent COMMAND testLibinputKeyEvent)
ecm_mark_as_test(testLibinputKeyEvent)

########################################################
# Test Pointer Event
########################################################
add_executable(testLibinputPointerEvent pointer_event_test.cpp)
target_link_libraries(testLibinputPointerEvent Qt::Test Qt::DBus Qt::Widgets KF6::ConfigCore LibInputTestObjects)
add_test(NAME kwin-testLibinputPointerEvent COMMAND testLibinputPointerEvent)
ecm_mark_as_test(testLibinputPointerEvent)

########################################################
# Test Touch Event
########################################################
add_executable(testLibinputTouchEvent touch_event_test.cpp)
target_link_libraries(testLibinputTouchEvent Qt::Test Qt::DBus Qt::Widgets KF6::ConfigCore LibInputTestObjects)
add_test(NAME kwin-testLibinputTouchEvent COMMAND testLibinputTouchEvent)
ecm_mark_as_test(testLibinputTouchEvent)

########################################################
# Test Gesture Event
########################################################
add_executable(testLibinputGestureEvent gesture_event_test.cpp)
target_link_libraries(testLibinputGestureEvent Qt::Test Qt::DBus Qt::Widgets KF6::ConfigCore LibInputTestObjects)
add_test(NAME kwin-testLibinputGestureEvent COMMAND testLibinputGestureEvent)
ecm_mark_as_test(testLibinputGestureEvent)
