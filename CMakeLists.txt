cmake_minimum_required(VERSION 3.20)

project(NBDisk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus Network)
find_package(KF6Config 6.0 REQUIRED)
find_package(KF6ConfigWidgets 6.0 REQUIRED)
find_package(KF6CoreAddons 6.0 REQUIRED)
find_package(KF6I18n 6.0 REQUIRED)
find_package(KF6Wallet 6.0 REQUIRED)
find_package(KF6WidgetsAddons 6.0 REQUIRED)
find_package(KF6XmlGui 6.0 REQUIRED)

add_executable(nbdisk
    src/backgroundserviceclient.cpp
    src/backgroundserviceclient.h
    src/backgroundservicedialog.cpp
    src/backgroundservicedialog.h
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/nbdcommand.cpp
    src/nbdcommand.h
    src/nbddevice.h
    src/nbddevicemodel.cpp
    src/nbddevicemodel.h
    src/nbdservice.cpp
    src/nbdservice.h
    src/secretstore.cpp
    src/secretstore.h
    src/servicedbus.h
    src/connectdialog.cpp
    src/connectdialog.h
    src/devicedetailswidget.cpp
    src/devicedetailswidget.h
    src/helperclient.cpp
    src/helperclient.h
    src/helperprotocol.h
    src/unlockdialog.cpp
    src/unlockdialog.h
    resources.qrc
)

target_compile_definitions(nbdisk PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    NBDISK_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(nbdisk PRIVATE
    Qt6::DBus
    Qt6::Network
    Qt6::Widgets
    KF6::ConfigWidgets
    KF6::CoreAddons
    KF6::I18n
    KF6::Wallet
    KF6::WidgetsAddons
    KF6::XmlGui
)

add_executable(nbdisk-daemon
    src/backgroundservice.cpp
    src/backgroundservice.h
    src/daemon.cpp
    src/servicedbus.h
    src/startuprunner.cpp
    src/startuprunner.h
    src/nbdcommand.cpp
    src/nbdcommand.h
    src/nbddevice.h
    src/nbdservice.cpp
    src/nbdservice.h
    src/helperclient.cpp
    src/helperclient.h
    src/helperprotocol.h
    src/secretstore.cpp
    src/secretstore.h
)

target_compile_definitions(nbdisk-daemon PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
)

target_link_libraries(nbdisk-daemon PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    KF6::I18n
    KF6::Wallet
)

add_executable(nbdisk-helper
    src/helper_main.cpp
    src/helperserver.cpp
    src/helperserver.h
    src/helperprotocol.h
    src/nbdcommand.cpp
    src/nbdcommand.h
    src/nbddevice.h
)

target_compile_definitions(nbdisk-helper PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
)

target_link_libraries(nbdisk-helper PRIVATE
    Qt6::Core
    Qt6::Network
    KF6::I18n
)

install(TARGETS nbdisk nbdisk-daemon nbdisk-helper
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/systemd/nbdisk-helper.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/nbdisk-helper.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nbdisk-helper.service
    DESTINATION lib/systemd/system)
install(FILES data/sysusers/nbdisk.conf DESTINATION lib/sysusers.d)
install(FILES data/io.github.liturondev.NBDisk.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.liturondev.NBDisk.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/io.github.liturondev.NBDisk.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/nbdiskui.rc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nbdisk)

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_executable(nbdisk-commandbuilder-test
        tests/nbdcommand_test.cpp
        src/nbdcommand.cpp
        src/nbddevice.h
    )
    target_include_directories(nbdisk-commandbuilder-test PRIVATE src)
    target_compile_definitions(nbdisk-commandbuilder-test PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    )
    target_link_libraries(nbdisk-commandbuilder-test PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(NAME nbdcommand_test COMMAND nbdisk-commandbuilder-test)

    add_executable(nbdisk-service-test
        tests/nbdservice_test.cpp
        src/nbdservice.cpp
        src/nbdservice.h
        src/nbdcommand.cpp
        src/nbdcommand.h
        src/nbddevice.h
        src/helperclient.cpp
        src/helperclient.h
        src/helperprotocol.h
        src/secretstore.cpp
        src/secretstore.h
    )
    target_include_directories(nbdisk-service-test PRIVATE src)
    target_compile_definitions(nbdisk-service-test PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    )
    target_link_libraries(nbdisk-service-test PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
        KF6::I18n
        KF6::Wallet
    )
    add_test(NAME nbdservice_test COMMAND nbdisk-service-test)

    add_executable(nbdisk-startuprunner-test
        tests/startuprunner_test.cpp
        src/startuprunner.cpp
        src/startuprunner.h
        src/nbdservice.cpp
        src/nbdservice.h
        src/nbdcommand.cpp
        src/nbdcommand.h
        src/nbddevice.h
        src/helperclient.cpp
        src/helperclient.h
        src/helperprotocol.h
        src/secretstore.cpp
        src/secretstore.h
    )
    target_include_directories(nbdisk-startuprunner-test PRIVATE src)
    target_compile_definitions(nbdisk-startuprunner-test PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    )
    target_link_libraries(nbdisk-startuprunner-test PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
        KF6::I18n
        KF6::Wallet
    )
    add_test(NAME startuprunner_test COMMAND nbdisk-startuprunner-test)
endif()
