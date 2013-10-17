grub2-bhyve
===========

The following packages are required to build:
* gcc47
* flex
* bison
* gmake
* gdb76 (for debugging)

The configure command line is:

    ./configure --with-platform=emu CC=gcc47 LEX=/usr/local/bin/flex

Running gmake will create a binary, grub-emu, in the grub-core directory.

The command syntax is

    grub-emu -r <root-dev> -m <device.map file> -M <guest-mem> <vmname>

The device.map file is a text file that describes the mappings between
grub device names and the filesystem images on the host e.g.
    (hd0)  /images/centos.iso
    (hd1)  /images/ubuntu-disk.img

There is an additional device, (host), that is always available and
allows the host filesystem to be accessed.

The -r parameter takes a device name from the device.map file which
will be used as the device for pathnames without a device specifier.

The -M parameter specifies the amount of bhyve guest memory in MBytes.

To boot a linux kernel, the 'linux' command is used to load the kernel
and specify command-line options, while the 'initrd' command is used
to load the initrd. The 'boot' command is then issued to finalize 
loading, set bhyve register state, and exit grub-emu.

    linux  /isolinux/vmlinuz text console=ttyS0 earlyprintk=serial debug
    initrd /isolinux/initrd.img
    boot

For OpenBSD, the command to load the kernel is 'kopenbsd'.

    kopenbsd -h com0 /bsd.mpm
    boot


