prefix=/usr
exec_prefix=${prefix}
libdir=${prefix}/@CMAKE_INSTALL_LIBDIR@
includedir=${prefix}/@CMAKE_INSTALL_INCLUDEDIR@

Name: rockchip_mpp
Description: Rockchip Media Process Platform
Requires.private:
Version: 1.0.0
Libs: -L${libdir} -lrockchip_mpp
Libs.private:
Cflags: -I${includedir} 
