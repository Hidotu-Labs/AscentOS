.SUFFIXES:

ARCH := x86_64
QEMUFLAGS := -m 2G

override IMAGE_NAME := ascentos-$(ARCH)

ASCENTD_CONFIG_FILES := \
	initrd/ascentd/default.target \
	initrd/ascentd/services/system-init.service \
	initrd/ascentd/services/console.service \
	initrd/ascentd/services/wayland.service \
	initrd/ascentd/services/x11.service

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# musl static sysroot (see scripts/musl-toolchain.sh). Built automatically for hello_musl / disk.img / run.
MUSL_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-musl/bin
MUSL_SYSROOT := $(CURDIR)/toolchain/musl-sysroot
MUSL_LIBC := $(MUSL_SYSROOT)/lib/libc.a
MUSL_CC ?= x86_64-linux-musl-gcc
MUSL_CXX ?= x86_64-linux-musl-g++
MUSL_USER_CFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib
MUSL_USER_CXXFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector -fno-exceptions -fno-rtti \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib

# glibc toolchain (see scripts/glibc-toolchain.sh)
GLIBC_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-glibc/bin
GLIBC_SYSROOT := $(CURDIR)/toolchain/glibc-sysroot
GLIBC_CC := $(GLIBC_TOOLCHAIN_BIN)/x86_64-buildroot-linux-gnu-gcc
GLIBC_USER_CFLAGS := -O2 -Wall -Wextra -fno-stack-protector \
	--sysroot=$(GLIBC_SYSROOT)

# Alpine rootfs sysroot (built by scripts/setup-alpine.sh)
ALPINE_SYSROOT := $(CURDIR)/build/alpine/rootfs

# GTK2 test - include/lib flags
GTK2_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include/gtk-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/gtk-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
	-I$(ALPINE_SYSROOT)/usr/include/cairo \
	-I$(ALPINE_SYSROOT)/usr/include/gdk-pixbuf-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/atk-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
	-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
	-I$(ALPINE_SYSROOT)/usr/include/libpng16
GTK2_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 \
	-lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
	-ljpeg -lmount -lblkid -leconf -lintl -lXrandr -lXinerama \
	-lgraphite2 -lXcomposite -lXdamage
GTK2_LDFLAGS := \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib

# GTK3 test - include/lib flags
GTK3_INCLUDES := \
	-I$(ALPINE_SYSROOT)/usr/include/gtk-3.0 \
	-I$(ALPINE_SYSROOT)/usr/include/glib-2.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/glib-2.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/pango-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/harfbuzz \
	-I$(ALPINE_SYSROOT)/usr/include/cairo \
	-I$(ALPINE_SYSROOT)/usr/include/gdk-pixbuf-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/atk-1.0 \
	-I$(ALPINE_SYSROOT)/usr/include/pixman-1 \
	-I$(ALPINE_SYSROOT)/usr/include/freetype2 \
	-I$(ALPINE_SYSROOT)/usr/include/libpng16 \
	-I$(ALPINE_SYSROOT)/usr/include/at-spi2-atk/2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/at-spi-2.0 \
	-I$(ALPINE_SYSROOT)/usr/include/dbus-1.0 \
	-I$(ALPINE_SYSROOT)/usr/lib/dbus-1.0/include \
	-I$(ALPINE_SYSROOT)/usr/include/epoxy
GTK3_LIBS := \
	-L$(ALPINE_SYSROOT)/usr/lib -L$(ALPINE_SYSROOT)/lib \
	-lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -latk-1.0 -latk-bridge-2.0 \
	-lcairo-gobject -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
	-lepoxy -ldbus-1 -lX11 -lXext -lXrender -lXi -lXcursor -lXfixes \
	-lwayland-client -lwayland-cursor -lwayland-egl \
	-lXrandr -lXinerama -lXcomposite -lXdamage \
	-lfontconfig -lfreetype -lpng16 -lz -lm
GTK3_LDFLAGS := \
	-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
	-Wl,-rpath,/usr/lib \
	-Wl,-rpath-link,$(ALPINE_SYSROOT)/usr/lib:$(ALPINE_SYSROOT)/lib

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: run
run: run-$(ARCH)

.PHONY: run-dist
run-dist: edk2-ovmf ascentos-dist.iso
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom ascentos-dist.iso \
		-m 2G \
		-serial stdio \
		$(QEMUFLAGS)

ascentos-dist.iso: limine/limine kernel disk.img
	./create_dist_usb.sh ascentos-dist.iso

.PHONY: run-x86_64
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso disk.img nvme.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=ide \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-drive file=nvme.img,if=none,id=nvm0 \
		-device nvme,drive=nvm0,serial=ascentos-nvme-0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-boot d \
		-audiodev pa,id=snd0 \
		-serial stdio \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

.PHONY: run-ata
run-ata: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M pc,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disk.img,format=raw,if=ide \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

# FAT32 test image for testing the FAT32 driver
fat32_test.img:
	./scripts/create-fat32-test.sh

.PHONY: run-fat32
run-fat32: edk2-ovmf $(IMAGE_NAME).iso fat32_test.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-hda fat32_test.img \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

# Create a 64MB ext2 disk image with sample files for testing
disk.img: scripts/configure-accounts.sh userland/ascent-account userland/test_accounts.sh userland/ascent-login.elf
disk.img:  userland/dns_lookup.elf
disk.img: userland/test_clone_futex.elf
disk.img: assets/boot.wav userland/test.c assets/test.wav assets/jane.mp3 assets/doom1.wad assets/mc9.mp3 assets/train.mp3 assets/test.bmp assets/test.tar assets/room.png assets/logo.png assets/linus.gif userland/forkit.elf userland/about.elf userland/hello_glibc.elf userland/booter.elf userland/reboot.elf userland/shutdown.elf userland/apm.elf userland/test_cpp.elf  userland/kilo.elf  userland/ls.elf userland/lspci.elf userland/lsblk.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/asplay.elf userland/kria.elf userland/doom.elf userland/xrootcursor.elf  userland/jwm.elf userland/doom_x11.elf userland/gtk_test.elf userland/tglgears_fb.elf userland/tglgears_drm.elf userland/tglhello_drm.elf userland/test_mem_stress.elf userland/classicube.elf userland/terrain.png userland/texpacks/classicube.zip initrd/startx.sh initrd/startw.sh initrd/weston.ini userland/ascentd.elf $(ASCENTD_CONFIG_FILES)
	@echo "Creating root filesystem (ext4)..."
	rm -f ./part.img
	dd if=/dev/zero of=./part.img bs=1M count=2047
	mkfs.ext4 -F -b 1024 -I 128 \
		-O extent,filetype,has_journal,dir_index,^64bit,^metadata_csum,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum_seed,^orphan_file \
		./part.img
	@echo "Populating root filesystem..."
	@{ \
		echo "cd /"; \
		echo "mkdir tmp"; \
		echo "mkdir bin"; \
		echo "mkdir lib"; \
		echo "rm bin/startx.sh"; \
		echo "write initrd/startx.sh bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "write initrd/startw.sh bin/startw.sh"; \
		echo "rm bin/ascentd"; \
		echo "write userland/ascentd.elf bin/ascentd"; \
		echo "rm bin/ascent-login"; \
		echo "write userland/ascent-login.elf bin/ascent-login"; \
		echo "rm bin/xrootcursor"; \
		echo "write userland/xrootcursor.elf bin/xrootcursor"; \
		echo "mkdir etc"; \
		echo "mkdir etc/ascentd"; \
		echo "mkdir etc/ascentd/services"; \
		echo "rm etc/ascentd/default.target"; \
		echo "write initrd/ascentd/default.target etc/ascentd/default.target"; \
		echo "rm etc/ascentd/services/system-init.service"; \
		echo "write initrd/ascentd/services/system-init.service etc/ascentd/services/system-init.service"; \
		echo "rm etc/ascentd/services/console.service"; \
		echo "write initrd/ascentd/services/console.service etc/ascentd/services/console.service"; \
		echo "rm etc/ascentd/services/wayland.service"; \
		echo "write initrd/ascentd/services/wayland.service etc/ascentd/services/wayland.service"; \
		echo "rm etc/ascentd/services/x11.service"; \
		echo "write initrd/ascentd/services/x11.service etc/ascentd/services/x11.service"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "rm lib/libc.so"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/libc.so"; \
		echo "rm lib/ld-musl-x86_64.so.1"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/ld-musl-x86_64.so.1"; \
		echo "mkdir lib64"; \
		echo "rm lib64/libc.so.6"; \
		echo "write toolchain/glibc-sysroot/lib/libc.so.6 lib64/libc.so.6"; \
		echo "rm lib64/libm.so.6"; \
		echo "write toolchain/glibc-sysroot/lib/libm.so.6 lib64/libm.so.6"; \
		echo "rm lib64/ld-linux-x86-64.so.2"; \
		echo "write toolchain/glibc-sysroot/lib/ld-linux-x86-64.so.2 lib64/ld-linux-x86-64.so.2"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d toolchain/glibc-sysroot/usr/include ]; then \
		echo "Installing GLIBC headers into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/usr/include usr/include; \
	fi
	@if [ -d toolchain/glibc-sysroot/usr/lib ]; then \
		echo "Installing GLIBC libs into /usr/lib64..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/usr/lib usr/lib64; \
	fi
	@{ \
		echo "cd /"; \
		echo "rm bin/netlink_test"; \
		echo "write userland/netlink_test.elf bin/netlink_test"; \
		echo "rm bin/kilo"; \
		echo "write userland/kilo.elf bin/kilo"; \
		echo "rm bin/about"; \
		echo "write userland/about.elf bin/about"; \
		echo "rm bin/hello_glibc"; \
		echo "write userland/hello_glibc.elf bin/hello_glibc"; \
		echo "rm bin/test_cpp"; \
		echo "write userland/test_cpp.elf bin/test_cpp"; \
		echo "rm bin/ls"; \
		echo "write userland/ls.elf bin/ls"; \
		echo "rm bin/lspci"; \
		echo "write userland/lspci.elf bin/lspci"; \
		echo "rm bin/lsblk"; \
		echo "write userland/lsblk.elf bin/lsblk"; \
		echo "rm bin/readelf"; \
		echo "write userland/readelf.elf bin/readelf"; \
		echo "rm bin/pong"; \
		echo "write userland/pong.elf bin/pong"; \
		echo "rm bin/raycast"; \
		echo "write userland/raycast.elf bin/raycast"; \
		echo "rm bin/kria"; \
		echo "write userland/kria.elf bin/kria"; \
		echo "rm bin/asplay"; \
		echo "write userland/asplay.elf bin/asplay"; \
		echo "rm bin/booter"; \
		echo "write userland/booter.elf bin/booter"; \
		echo "rm bin/reboot"; \
		echo "write userland/reboot.elf bin/reboot"; \
		echo "rm bin/shutdown"; \
		echo "write userland/shutdown.elf bin/shutdown"; \
		echo "rm bin/apm"; \
		echo "write userland/apm.elf bin/apm"; \
		echo "rm bin/test_cred"; \
		echo "write userland/test_cred.elf bin/test_cred"; \
		echo "rm test.s"; \
		echo "write userland/test.s test.s"; \
		echo "rm standalone.s"; \
		echo "write userland/standalone.s standalone.s"; \
		echo "rm test.wav"; \
		echo "write assets/test.wav test.wav"; \
		echo "rm boot.wav"; \
		echo "write assets/doom1.wad doom1.wad"; \
		echo "rm doom1.wad"; \
		echo "write userland/test.c test.c"; \
		echo "rm test.c"; \
		echo "write assets/boot.wav boot.wav"; \
		echo "rm jane.mp3"; \
		echo "write assets/jane.mp3 jane.mp3"; \
		echo "rm mc9.mp3"; \
		echo "write assets/mc9.mp3 mc9.mp3"; \
		echo "rm train.mp3"; \
		echo "write assets/train.mp3 train.mp3"; \
		echo "rm test.bmp"; \
		echo "write assets/test.bmp test.bmp"; \
		echo "mkdir assets"; \
		echo "rm assets/room.png"; \
		echo "write assets/room.png assets/room.png"; \
		echo "rm assets/logo.png"; \
		echo "write assets/logo.png assets/logo.png"; \
		echo "rm assets/linus.gif"; \
		echo "write assets/linus.gif assets/linus.gif"; \
		echo "rm test.krx"; \
		echo "write userland/kria-lang/test.krx test.krx"; \
		echo "rm hello.krx"; \
		echo "write userland/hello.krx hello.krx"; \
		echo "rm bin/doom"; \
		echo "write userland/doom.elf bin/doom"; \
		echo "rm bin/dns_lookup"; \
		echo "write userland/dns_lookup.elf bin/dns_lookup"; \
		echo "rm test.tar"; \
		echo "write assets/test.tar test.tar"; \
		echo "rm bin/tglgears"; \
		echo "write userland/tglgears_fb.elf bin/tglgears"; \
		echo "rm bin/tglgears_drm"; \
		echo "write userland/tglgears_drm.elf bin/tglgears_drm"; \
		echo "rm bin/tglhello_drm"; \
		echo "write userland/tglhello_drm.elf bin/tglhello_drm"; \
		echo "rm bin/test_mem_stress"; \
		echo "write userland/test_mem_stress.elf bin/test_mem_stress"; \
		echo "rm bin/test_clone_futex"; \
		echo "write userland/test_clone_futex.elf bin/test_clone_futex"; \
		echo "rm bin/classicube"; \
		echo "write userland/classicube.elf bin/classicube"; \
		echo "mkdir texpacks"; \
		echo "rm terrain.png"; \
		echo "write userland/terrain.png terrain.png"; \
		echo "rm texpacks/classicube.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/classicube.zip"; \
		echo "rm texpacks/default.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/default.zip"; \
		echo "rm bin/forkit.elf"; \
		echo "write userland/forkit.elf bin/forkit.elf"; \
		echo "rm bin/fault_mon"; \
		echo "write userland/fault_mon.elf bin/fault_mon"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Writing ClassiCube options.txt (texture pack config)..."
	@printf 'texture-pack=classicube.zip\nskin-server=\n' > /tmp/classicube_options.txt
	debugfs -w -R "rm options.txt" ./part.img >/dev/null 2>&1 || true
	debugfs -w -R "write /tmp/classicube_options.txt options.txt" ./part.img >/dev/null 2>&1 || true
	rm -f /tmp/classicube_options.txt
	rm -f /tmp/ascentos_hello.txt /tmp/ascentos_readme.txt
	@echo "Installing Forkit assets (fonts + test pages) into disk image..."
	@{ \
		echo "cd /"; \
		echo "mkdir usr"; \
		echo "mkdir usr/share"; \
		echo "mkdir usr/share/forkit"; \
		echo "mkdir usr/share/forkit/assets"; \
		echo "mkdir usr/share/forkit/assets/fonts"; \
		echo "rm usr/share/forkit/assets/test.html"; \
		echo "write build/forkit/assets/test.html usr/share/forkit/assets/test.html"; \
		echo "rm usr/share/forkit/assets/html-test.html"; \
		echo "write build/forkit/assets/html-test.html usr/share/forkit/assets/html-test.html"; \
		echo "rm usr/share/forkit/assets/css-test.html"; \
		echo "write build/forkit/assets/css-test.html usr/share/forkit/assets/css-test.html"; \
		echo "rm usr/share/forkit/assets/js-test.html"; \
		echo "write build/forkit/assets/js-test.html usr/share/forkit/assets/js-test.html"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Regular.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Regular.ttf usr/share/forkit/assets/fonts/NotoSans-Regular.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Bold.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Bold.ttf usr/share/forkit/assets/fonts/NotoSans-Bold.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-Italic.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-Italic.ttf usr/share/forkit/assets/fonts/NotoSans-Italic.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSans-BoldItalic.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSans-BoldItalic.ttf usr/share/forkit/assets/fonts/NotoSans-BoldItalic.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSansMono-Regular.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSansMono-Regular.ttf usr/share/forkit/assets/fonts/NotoSansMono-Regular.ttf"; \
		echo "rm usr/share/forkit/assets/fonts/NotoSansMono-Bold.ttf"; \
		echo "write build/forkit/assets/fonts/NotoSansMono-Bold.ttf usr/share/forkit/assets/fonts/NotoSansMono-Bold.ttf"; \
		echo "rm bin/forkit"; \
		echo "write userland/forkit-launch.sh bin/forkit"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@if [ -d build/alpine/rootfs ]; then \
		echo "Populating Alpine Linux rootfs into disk image..."; \
		./scripts/configure-accounts.sh build/alpine/rootfs; \
		./scripts/populate-ext2-dir.sh ./part.img build/alpine/rootfs /; \
	fi
	@echo "Fixing up glibc/musl library coexistence..."
	@{ \
		echo "cd /lib"; \
		echo "rm libc.so.6"; \
		echo "rm libm.so.6"; \
		echo "rm libpthread.so.0"; \
		echo "rm ld-linux-x86-64.so.2"; \
		echo "rm libresolv.so.2"; \
		echo "rm librt.so.1"; \
		echo "rm libutil.so.1"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@# Restore real glibc libs to /lib so glibc-linked binaries (coreutils sleep,
	@# mkdir etc.) resolve symbols correctly. musl stub at /lib/libc.so.6 lacks
	@# glibc-specific symbols like re_syntax_options which crashes those binaries.
	@if [ -f toolchain/glibc-sysroot/lib/libc.so.6 ]; then \
		echo "Restoring real glibc libc.so.6 to /lib..."; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/libc.so.6 lib/libc.so.6" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/libm.so.6 lib/libm.so.6" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/lib/ld-linux-x86-64.so.2 lib/ld-linux-x86-64.so.2" ./part.img >/dev/null 2>&1 || true; \
	fi
	@echo "Populating root filesystem with additional tools..."

	@if [ -d build/tcc-glibc-install/opt/tcc ]; then \
		echo "Installing GLIBC TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img build/tcc-glibc-install/opt/tcc opt/tcc; \
		debugfs -w -R "rm bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm lib64/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/tcc-glibc-install/opt/tcc/lib/libtcc.so lib64/libtcc.so" ./part.img >/dev/null 2>&1 || true; \
	elif [ -d toolchain/musl-sysroot/opt/tcc ]; then \
		echo "Installing MUSL TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/tcc opt/tcc; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/libc.a libc.a" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crt1.o crt1.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crti.o crti.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crtn.o crtn.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/lib/tcc/libtcc1.a libtcc1.a" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -d toolchain/glibc-sysroot/opt/coreutils ]; then \
		echo "Installing glibc coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/opt/coreutils opt/coreutils; \
	elif [ -d toolchain/musl-sysroot/opt/coreutils ]; then \
		echo "Installing musl coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/coreutils opt/coreutils; \
	fi
	@if [ -d toolchain/glibc-sysroot/opt/bash ]; then \
		echo "Installing bash into disk image..."; \
		debugfs -w -R "mkdir opt" ./part.img >/dev/null 2>&1 || true; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/glibc-sysroot/opt/bash opt/bash; \
		debugfs -w -R "rm bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/opt/bash/bin/bash bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm bin/sh" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/glibc-sysroot/opt/bash/bin/bash bin/sh" ./part.img >/dev/null 2>&1 || true; \
		echo "PS1='\033[0;32mRoot@AscentOS\033[0m:\w\\$$ '" > /tmp/bashrc; \
		echo "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/opt/coreutils/bin:/usr/bin:/sbin:/bin:/opt/bash/bin:/opt/tcc/bin" >> /tmp/bashrc; \
		echo "HOME=/" >> /tmp/bashrc; \
		echo "TERM=xterm-256color" >> /tmp/bashrc; \
		echo "export TERM" >> /tmp/bashrc; \
		echo "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt" >> /tmp/bashrc; \
		echo "SSL_CERT_DIR=/etc/ssl/certs" >> /tmp/bashrc; \
		echo "CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt" >> /tmp/bashrc; \
		echo "export SSL_CERT_FILE SSL_CERT_DIR CURL_CA_BUNDLE" >> /tmp/bashrc; \
		debugfs -w -R "mkdir etc" ./part.img >/dev/null 2>&1 || true; \
		echo "nameserver 10.0.2.3" > /tmp/resolv.conf; \
		echo "127.0.0.1 localhost" > /tmp/hosts; \
		debugfs -w -R "rm etc/resolv.conf" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/resolv.conf etc/resolv.conf" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/hosts" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/hosts etc/hosts" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir etc/ssl" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir etc/ssl/certs" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/ssl/certs/ca-certificates.crt" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write build/alpine/rootfs/etc/ssl/certs/ca-certificates.crt etc/ssl/certs/ca-certificates.crt" ./part.img >/dev/null 2>&1 || true; \
		echo "NAME=\"AscentOS\"" > /tmp/os-release; \
		echo "PRETTY_NAME=\"AscentOS 2.0.0 Beta x86_64\"" >> /tmp/os-release; \
		echo "ID=ascentos" >> /tmp/os-release; \
		echo "VERSION_ID=2.0.0 Beta" >> /tmp/os-release; \
		echo "HOME_URL=\"https://github.com/AscentOS\"" >> /tmp/os-release; \
		debugfs -w -R "rm etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/os-release etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm .bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/bashrc .bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir .config" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir .config/fastfetch" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir fastfetch" ./part.img >/dev/null 2>&1 || true; \
		echo '{"logo": {"source": "/fastfetch/logo.txt", "type": "auto"}, "modules": ["title", "separator", "os", "host", "kernel", "uptime", "packages", {"type": "shell", "format": "bash"}, "display", "de", "wm", "wmtheme", "theme", "icons", "font", "cursor", "terminal", "terminalfont", "cpu", "gpu", "memory", "swap", "disk", "battery", "poweradapter", "locale", "break", "colors"]}' > /tmp/ff_config.jsonc; \
		debugfs -w -R "rm .config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc .config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write assets/ascii-art.txt fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		rm -f /tmp/bashrc /tmp/resolv.conf /tmp/hosts /tmp/ff_config.jsonc /tmp/os-release; \
	fi
	@if [ -f userland/icewmrc ] && [ -f userland/winoptions ]; then \
		echo "Installing IceWM configuration into disk image..."; \
		debugfs -w -R "mkdir etc/icewm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/icewmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/icewmrc etc/icewm/icewmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/winoptions etc/icewm/winoptions" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f toolchain/musl-sysroot/bin/tar ]; then \
		echo "Installing tar into disk image..."; \
		debugfs -w -R "rm bin/tar" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/bin/tar bin/tar" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/xeyes.elf ]; then \
		echo "Installing xeyes into disk image..."; \
		debugfs -w -R "rm xeyes" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/xeyes.elf xeyes" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/twm.elf ]; then \
		echo "Installing twm into disk image..."; \
		debugfs -w -R "rm twm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/twm.elf twm" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/jwm.elf ]; then \
		echo "Installing jwm into disk image..."; \
		debugfs -w -R "rm bin/jwm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/jwm.elf bin/jwm" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm .jwmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/jwmrc .jwmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm bg.png" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write assets/room.png bg.png" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/doom_x11.elf ]; then \
		echo "Installing doom_x11 into disk image..."; \
		debugfs -w -R "rm bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/doom_x11.elf bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
	fi

	@if [ -f userland/xeyes.elf ] || [ -f userland/twm.elf ]; then \
		debugfs -w -R "rm .Xauthority" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write initrd/.Xauthority .Xauthority" ./part.img >/dev/null 2>&1 || true; \
	fi
	@echo "Fixing executable modes for directly injected launchers..."
	@{ \
		echo "rm bin/ls"; \
		echo "symlink bin/ls /opt/coreutils/bin/ls"; \
		echo "set_inode_field bin/forkit mode 0100755"; \
		echo "set_inode_field bin/test_cred mode 0100755"; \
		echo "set_inode_field bin/test_accounts mode 0100755"; \
		echo "set_inode_field home/ascent uid 1000"; \
		echo "set_inode_field home/ascent gid 1000"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Creating partitioned disk image (MBR)..."
	dd if=/dev/zero of=disk.img bs=1M count=2048
	parted -s disk.img mklabel msdos
	parted -s disk.img mkpart primary ext3 1MiB 100%
	parted -s disk.img set 1 boot on
	dd if=./part.img of=disk.img bs=1M seek=1 conv=notrunc
	rm ./part.img
	@touch disk.img

nvme.img:
	dd if=/dev/zero of=nvme.img bs=1M count=128

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch=v11.x-binary --depth=1
	$(MAKE) -C limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

.PHONY: setup
setup:
	chmod +x bootstrap.sh
	./bootstrap.sh

.PHONY: kernel
kernel: setup
	$(MAKE) -C kernel

$(IMAGE_NAME).iso: limine/limine kernel
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf iso_root/boot/limine/
	cp -v assets/boo.png iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -f $(IMAGE_NAME).iso

.PHONY: clean-all
clean-all: clean-musl clean-doom clean-coreutils clean-tar
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd nvme.img build/alpine

.PHONY: clean-coreutils
clean-coreutils:
	rm -rf build/coreutils-9.5
	rm -rf toolchain/musl-sysroot/opt/coreutils
	rm -rf toolchain/glibc-sysroot/opt/coreutils

.PHONY: clean-musl
clean-musl:
	rm -rf build/musl-1.2.5 build/musl-cross-make
	rm -rf toolchain/musl-sysroot toolchain/x86_64-linux-musl
	rm -f userland/hello.elf userland/ascentd.elf  userland/kilo.elf userland/kilo.c  userland/asplay.elf userland/kria.elf userland/ls.elf userland/readelf.elf userland/poll_test.elf
	rm -rf userland/kria-lang/target

.PHONY: clean-disk
clean-disk:
	rm -f disk.img

.PHONY: distclean
distclean: clean-musl clean-doom
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd limine edk2-ovmf doomgeneric
	rm -rf userland/kria-lang

# ── Userland test programs ──────────────────────────────────────────────────
$(MUSL_LIBC):
	chmod +x scripts/musl-toolchain.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" ./scripts/musl-toolchain.sh

.PHONY: musl-toolchain
musl-toolchain: $(MUSL_LIBC)

.PHONY: test-phase6-login
test-phase6-login:
	./scripts/test-phase6-login.sh

userland/ascentd.elf: userland/ascentd.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ascentd.c -o userland/ascentd.elf

userland/ascent-login.elf: userland/ascent-login.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ascent-login.c -lcrypt -o userland/ascent-login.elf

userland/hello_glibc.elf: userland/hello_glibc.c
	$(GLIBC_CC) $(GLIBC_USER_CFLAGS) \
		userland/hello_glibc.c -o userland/hello_glibc.elf

userland/test_cpp.elf: userland/test_cpp.cpp $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CXX) $(MUSL_USER_CXXFLAGS) \
		userland/test_cpp.cpp -o userland/test_cpp.elf


userland/kilo.c:
	curl -L https://raw.githubusercontent.com/antirez/kilo/master/kilo.c -o userland/kilo.c

userland/kilo.elf: userland/kilo.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/kilo.c -o userland/kilo.elf

userland/ls.elf: userland/ls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ls.c -o userland/ls.elf

userland/lspci.elf: userland/lspci.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/lspci.c -o userland/lspci.elf

userland/lsblk.elf: userland/lsblk.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/lsblk.c -o userland/lsblk.elf

userland/readelf.elf: userland/readelf.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/readelf.c -o userland/readelf.elf

userland/pong.elf: userland/pong.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/pong.c -o userland/pong.elf

userland/raycast.elf: userland/raycast.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/raycast.c -lm -o userland/raycast.elf

userland/asplay.elf: userland/asplay.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/asplay.c -o userland/asplay.elf

userland/booter.elf: userland/booter.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/booter.c -o userland/booter.elf

userland/reboot.elf: userland/reboot.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/reboot.c -o userland/reboot.elf

userland/shutdown.elf: userland/shutdown.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/shutdown.c -o userland/shutdown.elf

userland/apm.elf: userland/apm.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/apm.c -o userland/apm.elf

userland/dns_lookup.elf: userland/dns_lookup.c userland/dns_resolver.c userland/dns_resolver.h $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/dns_lookup.c userland/dns_resolver.c -o userland/dns_lookup.elf

# Kria programming language (Rust-based, compiled with musl for static linking)
userland/kria-lang:
	rm -rf userland/kria-lang
	git clone https://github.com/Piotriox/kria-lang.git userland/kria-lang
	mkdir -p userland/kria-lang/.cargo
	echo '[build]' > userland/kria-lang/.cargo/config.toml
	echo 'target = "x86_64-unknown-linux-musl"' >> userland/kria-lang/.cargo/config.toml

userland/kria.elf: userland/kria-lang
	cd userland/kria-lang && cargo build --release
	cp userland/kria-lang/target/x86_64-unknown-linux-musl/release/kria userland/kria.elf

.PHONY: kria
kria: userland/kria.elf

# ── DOOM (doomgeneric) ──────────────────────────────────────────────────────
.PHONY: doom
doom: userland/doom.elf

doomgeneric:
	rm -rf doomgeneric
	git clone https://github.com/ozkl/doomgeneric.git --depth=1

userland/doom.elf: doomgeneric $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MAKE) -C userland -f Makefile.ascentos \
		MUSL_CC="$(MUSL_CC)" \
		MUSL_SYSROOT="$(MUSL_SYSROOT)"

.PHONY: clean-doom
clean-doom:
	$(MAKE) -C userland -f Makefile.ascentos clean

userland/gtk_test.elf: userland/gtk_test.c scripts/setup-alpine.sh
	@if [ ! -d "$(ALPINE_SYSROOT)" ]; then \
		echo "Error: Alpine rootfs not found. Run scripts/setup-alpine.sh first."; \
		exit 1; \
	fi
	@echo "[*] Compiling userland/gtk_test.c (GTK2) ..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc -O2 \
		userland/gtk_test.c \
		-o userland/gtk_test.elf \
		$(GTK2_INCLUDES) \
		$(GTK2_LIBS) \
		$(GTK2_LDFLAGS)

userland/gtk3_test.elf: userland/gtk3_test.c scripts/setup-alpine.sh
	@if [ ! -d "$(ALPINE_SYSROOT)" ]; then \
		echo "Error: Alpine rootfs not found. Run scripts/setup-alpine.sh first."; \
		exit 1; \
	fi
	@echo "[*] Compiling userland/gtk3_test.c (GTK3) ..."
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" \
		$(MUSL_TOOLCHAIN_BIN)/x86_64-linux-musl-gcc -O2 \
		userland/gtk3_test.c \
		-o userland/gtk3_test.elf \
		$(GTK3_INCLUDES) \
		$(GTK3_LIBS) \
		$(GTK3_LDFLAGS)

userland/tglgears_fb.elf: userland/tglgears_fb.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_fb.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -L$(MUSL_SYSROOT)/lib -lX11 -lxcb -lXau -lXdmcp -lm -o userland/tglgears_fb.elf

userland/test_mem_stress.elf: userland/test_mem_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mem_stress.c -o userland/test_mem_stress.elf

userland/test_clone_futex.elf: userland/test_clone_futex.c userland/test_clone_futex_trampoline.S $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_clone_futex.c userland/test_clone_futex_trampoline.S -o userland/test_clone_futex.elf

userland/panic_test.elf: userland/panic_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/panic_test.c -o userland/panic_test.elf

userland/fault_mon.elf: userland/fault_mon.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/fault_mon.c -o userland/fault_mon.elf

userland/tglgears_drm.elf: userland/tglgears_drm.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglgears_drm.elf

userland/tglhello_drm.elf: userland/tglhello_drm.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglhello_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglhello_drm.elf

userland/classicube.elf: scripts/build-classicube.sh
	./scripts/build-classicube.sh

userland/forkit.elf: scripts/build-forkit.sh
	./scripts/build-forkit.sh

userland/about.elf: userland/about.c scripts/build-about.sh scripts/setup-alpine.sh
	./scripts/build-about.sh

userland/texpacks/classicube.zip:
	@echo "ERROR: userland/texpacks/classicube.zip not found."
	@echo "Please place the original ClassiCube texture pack zip at: userland/texpacks/classicube.zip"
	@exit 1

userland/terrain.png: userland/texpacks/classicube.zip
	@echo "Extracting terrain.png from classicube.zip..."
	cd userland && unzip -o texpacks/classicube.zip terrain.png

.PHONY: all qemu clean

# ── addr2line helper ──────────────────────────────────────────────────────────
# Resolve one or more kernel addresses from the serial log to source locations.
#
# Usage:
#   make addr2line ADDRS="0xffffffff80012abc 0xffffffff80034def"
#
# Or pipe addresses extracted from the log directly:
#   grep -oP '(?<=caller=|TRACE #\d\] )0x[0-9A-Fa-f]+' serial.log | \
#       xargs make addr2line ADDRS=
#
# The kernel binary keeps full DWARF info (-g) so addr2line gives exact
# file:line and function name for every address.
KERNEL_BIN := kernel/bin-x86_64/kernel
ADDRS ?=

.PHONY: addr2line
addr2line:
	@if [ -z "$(ADDRS)" ]; then \
		echo "Usage: make addr2line ADDRS=\"0xffffffff80012abc 0xffffffff80034def\""; \
		echo ""; \
		echo "Quick extract from serial.log:"; \
		echo "  grep -oP '(?<=caller=|\\[TRACE #[0-9]+\\] )0x[0-9A-Fa-f]+' serial.log | sort -u | xargs -I{} make addr2line ADDRS={}"; \
		exit 0; \
	fi
	@echo "=== addr2line: $(KERNEL_BIN) ==="
	@for addr in $(ADDRS); do \
		echo -n "$$addr  ->  "; \
		addr2line -e $(KERNEL_BIN) -f -p "$$addr" 2>/dev/null || echo "(not found)"; \
	done

# Convenience: extract ALL [HERE]/[TRACE] addresses from serial.log and resolve them
.PHONY: resolve-log
resolve-log:
	@echo "=== Resolving all [HERE]/[TRACE] addresses from serial.log ==="
	@grep -oP '(?<=(caller=|\] ))0x[0-9A-Fa-f]+' serial.log 2>/dev/null | sort -u | while read addr; do \
		echo -n "$$addr  ->  "; \
		addr2line -e $(KERNEL_BIN) -f -p "$$addr" 2>/dev/null || echo "(not found)"; \
	done
