grub2-bhyve
===========

The following packages are required to build:
* gcc
* flex
* bison
* gmake

Optionally:
* gdb (for debugging)

The configure command line is:

    ./configure --with-platform=emu CC=gcc LEX=/usr/local/bin/flex \
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
Optionally a partition can also be specified, separated by a comma,
like "hd0,msdos1".
If a grub.cfg file is found on the specified partition it will be
loaded automatically.

The -M parameter specifies the amount of bhyve guest memory in MBytes.

The -n parameter disables auto-insertion of "console=ttyS0" to the
 start of the Linux kernel command-line.

The -S parameter forces wiring of guest memory on FreeBSD-11 hosts.
This is required for PCI passthru.

To boot a linux kernel, the 'linux' command is used to load the kernel
and specify command-line options, while the 'initrd' command is used
to load the initrd. The 'boot' command is then issued to finalize 
loading, set bhyve register state, and exit grub-emu.
grub-emu will auto-insert a "console=ttyS0" line if there isn't one
present in the command line. This can be disabled by passing the
'-n' option to grub-emu.
If the kernel is loaded from an ext2/3/4 partittion on disk and not iso,
you will need to load the "ext2" module using the "insmod ext2" command.
If you have a working grub.cfg on disk, avoid needing to issue commands
at the grub prompt by specifying the relevant partition and root device
when envoking grub-emu.


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

