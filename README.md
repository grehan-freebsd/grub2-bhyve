grub2-bhyve
===========

The following packages are required to build:
* gcc (installs gcc48)
* flex
* bison
* gmake

Optionally:
* gdb78 (for debugging)

The configure command line is:

    ./configure --with-platform=emu CC=gcc48 LEX=/usr/local/bin/flex \
        --enable-grub-mount=no --enable-grub-mkfont=no \
	--enable-grub-emu-sdl=no --disable-nls --disable-werror

Running gmake will create a binary, grub-emu, in the grub-core directory.

The command syntax is

    grub-emu -r <root-dev> -m <device.map file> [-n] -M <guest-mem> <vmname>

The device.map file is a text file that describes the mappings between
grub device names and the filesystem images on the host e.g.

    (cd0)  /images/centos.iso
    (hd1)  /images/ubuntu-disk.img

There is an additional device, "(host)", that is always available and
allows the host filesystem to be accessed.

The -r parameter takes a device name from the device.map file which
will be used as the device for pathnames without a device specifier.

The -M parameter specifies the amount of bhyve guest memory in MBytes.

The -n parameter disables auto-insertion of "console=ttyS0" to the
 start of the Linux kernel command-line.

To boot a linux kernel, the 'linux' command is used to load the kernel
and specify command-line options, while the 'initrd' command is used
to load the initrd. The 'boot' command is then issued to finalize 
loading, set bhyve register state, and exit grub-emu.
grub-emu will auto-insert a "console=ttyS0" line if there isn't one
present in the command line. This can be disabled by passing the
'-n' option to grub-emu.

    linux  /isolinux/vmlinuz text earlyprintk=serial debug
    initrd /isolinux/initrd.img
    boot

For OpenBSD i386/amd64, the command to load the kernel is 'kopenbsd'. The
"-h com0" option forces the use of the serial console - this should always
be used with bhyve. The root device is specified with the "-r <sdX|wdX>"
parameter.

    kopenbsd -h com0 -r sd0a /bsd
    boot

NetBSD/amd64 is booted with the 'knetbsd' command. Similar to OpenBSD,
the "-h" and "-r" parameters should be used to specify a serial console
and the root device.

    knetbsd -h -r cd0a /netbsd
    boot 

FreeBSD/amd64 can be booted using the kfreebsd command. Note that
grub will not automatically source any of the FreeBSD loader variable
files, or interpret these as the FreeBSD loader does. The boot process
will have to be manual unless a grub.cfg file has been created.

    kfreebsd -h /boot/kernel/kernel
    kfreebsd_loadenv /boot/device.hints
    boot

