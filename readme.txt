Building on Ubuntu.

1. It's need to install CodeSourcery's toolchain from https://sourcery.mentor.com/sgpp/lite/arm/portal/package1791/public/arm-none-linux-gnueabi/arm-2007q3-51-arm-none-linux-gnueabi.bin

2. Install build environments: 
    sudo apt-get install build-essential subversion bison flex texinfo tcl8.3-dev tk8.3-dev bwidget

3. Download Miniemc2 source code from repository:
    svn checkout http://miniemc2.googlecode.com/svn/trunk/ miniemc2-read-only

4. Change current directory to trunc/buildroot and enter: 
    make menuconfig
Modify path to CodeSourcery toolchain editing variable at Toolchain->External toolchain path
Save change and exit.

5. Start building entering
    make
It takes some time, during it you will be asked for superuser password used in Xenomai's tests.

6. uboot, kernel and root filesystem's Images will be avalibale at trunk/buildroot/output/images/ folder.


