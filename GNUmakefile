.SUFFIXES:

ARCH := x86_64
QEMUFLAGS := -m 2G

override IMAGE_NAME := ascentos-$(ARCH)

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# musl static sysroot (see scripts/musl-toolchain.sh). Built automatically for hello_musl / disk.img / run.
MUSL_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-musl/bin
MUSL_SYSROOT := $(CURDIR)/toolchain/musl-sysroot
MUSL_LIBC := $(MUSL_SYSROOT)/lib/libc.a
WOLFSSL_LIB := $(MUSL_SYSROOT)/lib/libwolfssl.a
MUSL_CC ?= x86_64-linux-musl-gcc
MUSL_CXX ?= x86_64-linux-musl-g++
MUSL_USER_CFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib
MUSL_USER_CXXFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector -fno-exceptions -fno-rtti \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: run
run: run-$(ARCH)

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
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-netdev user,id=net0 \
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
		-device e1000,netdev=net0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-netdev user,id=net0 \
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
		-device e1000,netdev=net0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-device intel-hda -device hda-duplex,audiodev=snd0 \
		-netdev user,id=net0 \
		-device usb-ehci,id=ehci \
		-device usb-tablet,bus=ehci.0 \
		-device usb-kbd,bus=ehci.0 \
		$(QEMUFLAGS)

# Create a 64MB ext2 disk image with sample files for testing
disk.img: assets/test.wav assets/test.bmp assets/test.tar assets/room.png userland/hello.elf userland/test_cpp.elf userland/test_cow.elf userland/test_syscalls.elf userland/test_kilo_syscalls.elf userland/test_wait4_complex.elf userland/kilo.elf userland/test_args.elf userland/test_stat.elf userland/ls.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/test_mmap_shared_private.elf userland/playwav.elf userland/showbmp.elf userland/test_uname_pipe.elf userland/test_pipe_fork.elf userland/test_sys_access.elf userland/test_sys_cwd.elf userland/test_newfstatat.elf userland/test_unlink_rename.elf userland/wget.elf userland/kria.elf userland/doom.elf userland/poll_test.elf userland/pty_test.elf userland/test_tcc_libc.c userland/test_mm.c userland/test_dynamic.elf userland/test_dup.elf userland/test_attrib.elf userland/test_symlink.elf userland/test_cred.elf userland/test_time.elf userland/test_tsc_manual.elf userland/test_unix_sock.elf userland/test_unix_fdpass.elf userland/test_fb.elf userland/test_events.elf userland/test_read.elf userland/test_socket_phase3.elf userland/test_socket_phase3_advanced.elf userland/test_socket_phase3_megastress.elf userland/test_socket_phase4.elf userland/test_socket_phase5.elf userland/test_socket_phase6.elf userland/test_socket_phase7.elf userland/test_socket_phase7_advanced.elf userland/test_socket_phase8.elf userland/test_socket_phase9.elf userland/test_socket_phase10.elf userland/test_socket_phase11.elf userland/test_inet_stress.elf userland/dns_lookup.elf userland/test_signal_subsystem.elf userland/xeyes.elf userland/test_x11_simple.elf userland/xkbcomp.elf userland/test_shared_irq.elf userland/jwm.elf userland/doom_x11.elf userland/gtk_test.elf userland/tinywl.elf userland/tglgears_fb.elf userland/tglgears_drm.elf userland/tglhello_drm.elf userland/drm_bench.elf userland/test_drm_kms.elf userland/test_drm_flip.elf userland/test_drm_atomic.elf userland/test_drm_epoll.elf userland/test_clone_futex.elf userland/test_clone_futex_stress.elf userland/test_mem_stress.elf userland/test_io_leak.elf userland/classicube.elf userland/terrain.png userland/texpacks/classicube.zip userland/netlink_test.elf initrd/startx.sh initrd/weston.ini
	@echo "Creating root filesystem (ext3)..."
	rm -f ./part.img
	dd if=/dev/zero of=./part.img bs=1M count=2047
	mkfs.ext3 -F -b 1024 -I 128 ./part.img
	@echo "Populating root filesystem..."
	@echo "Hello from AscentOS!" > /tmp/ascentos_hello.txt
	@echo "This is a test document." > /tmp/ascentos_readme.txt
	@{ \
		echo "cd /"; \
		echo "mkdir tmp"; \
		echo "mkdir bin"; \
		echo "mkdir lib"; \
		echo "rm hello.txt"; \
		echo "write /tmp/ascentos_hello.txt hello.txt"; \
		echo "rm bin/startx.sh"; \
		echo "write initrd/startx.sh bin/startx.sh"; \
		echo "rm bin/startw.sh"; \
		echo "write initrd/startw.sh bin/startw.sh"; \
		echo "rm bin/tinywl"; \
		echo "write userland/tinywl.elf bin/tinywl"; \
		echo "mkdir etc"; \
		echo "rm etc/weston.ini"; \
		echo "write initrd/weston.ini etc/weston.ini"; \
		echo "rm lib/libc.so"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/libc.so"; \
		echo "rm lib/ld-musl-x86_64.so.1"; \
		echo "write toolchain/musl-sysroot/lib/libc.so lib/ld-musl-x86_64.so.1"; \
		echo "rm bin/test_syscalls"; \
		echo "write userland/test_syscalls.elf bin/test_syscalls"; \
		echo "rm bin/test_kilo_syscalls"; \
		echo "write userland/test_kilo_syscalls.elf bin/test_kilo_syscalls"; \
		echo "rm bin/netlink_test"; \
		echo "write userland/netlink_test.elf bin/netlink_test"; \
		echo "rm bin/test_wait4_complex"; \
		echo "write userland/test_wait4_complex.elf bin/test_wait4_complex"; \
		echo "rm bin/kilo"; \
		echo "write userland/kilo.elf bin/kilo"; \
		echo "rm bin/test_args"; \
		echo "write userland/test_args.elf bin/test_args"; \
		echo "rm bin/hello"; \
		echo "write userland/hello.elf bin/hello"; \
		echo "rm bin/test_cpp"; \
		echo "write userland/test_cpp.elf bin/test_cpp"; \
		echo "rm bin/test_cow"; \
		echo "write userland/test_cow.elf bin/test_cow"; \
		echo "rm bin/test_stat"; \
		echo "write userland/test_stat.elf bin/test_stat"; \
		echo "rm bin/ls"; \
		echo "write userland/ls.elf bin/ls"; \
		echo "rm bin/readelf"; \
		echo "write userland/readelf.elf bin/readelf"; \
		echo "rm bin/pong"; \
		echo "write userland/pong.elf bin/pong"; \
		echo "rm bin/raycast"; \
		echo "write userland/raycast.elf bin/raycast"; \
		echo "rm bin/test_mmap_shared_private"; \
		echo "write userland/test_mmap_shared_private.elf bin/test_mmap_shared_private"; \
		echo "rm bin/kria"; \
		echo "write userland/kria.elf bin/kria"; \
		echo "rm bin/playwav"; \
		echo "write userland/playwav.elf bin/playwav"; \
		echo "rm bin/showbmp"; \
		echo "write userland/showbmp.elf bin/showbmp"; \
		echo "rm bin/test_uname_pipe"; \
		echo "write userland/test_uname_pipe.elf bin/test_uname_pipe"; \
		echo "rm bin/test_pipe_fork"; \
		echo "write userland/test_pipe_fork.elf bin/test_pipe_fork"; \
		echo "rm bin/test_sys_access"; \
		echo "write userland/test_sys_access.elf bin/test_sys_access"; \
		echo "rm bin/test_sys_cwd"; \
		echo "write userland/test_sys_cwd.elf bin/test_sys_cwd"; \
		echo "rm bin/test_newfstatat"; \
		echo "write userland/test_newfstatat.elf bin/test_newfstatat"; \
		echo "rm bin/test_unlink_rename"; \
		echo "write userland/test_unlink_rename.elf bin/test_unlink_rename"; \
		echo "rm bin/test_ext3"; \
		echo "write userland/test_ext3.elf bin/test_ext3"; \
		echo "rm bin/test_dynamic"; \
		echo "write userland/test_dynamic.elf bin/test_dynamic"; \
		echo "rm test_tcc_libc.c"; \
		echo "write userland/test_tcc_libc.c test_tcc_libc.c"; \
		echo "rm test_mm.c"; \
		echo "write userland/test_mm.c test_mm.c"; \
		echo "rm bin/test_dup"; \
		echo "write userland/test_dup.elf bin/test_dup"; \
		echo "rm bin/test_attrib"; \
		echo "write userland/test_attrib.elf bin/test_attrib"; \
		echo "rm bin/test_symlink"; \
		echo "write userland/test_symlink.elf bin/test_symlink"; \
		echo "rm bin/test_cred"; \
		echo "write userland/test_cred.elf bin/test_cred"; \
		echo "rm test.s"; \
		echo "write userland/test.s test.s"; \
		echo "rm standalone.s"; \
		echo "write userland/standalone.s standalone.s"; \
		echo "rm test.wav"; \
		echo "write assets/test.wav test.wav"; \
		echo "rm test.bmp"; \
		echo "write assets/test.bmp test.bmp"; \
		echo "mkdir assets"; \
		echo "rm assets/room.png"; \
		echo "write assets/room.png assets/room.png"; \
		echo "rm test.krx"; \
		echo "write userland/kria-lang/test.krx test.krx"; \
		echo "rm hello.krx"; \
		echo "write userland/hello.krx hello.krx"; \
		echo "rm bin/doom"; \
		echo "write userland/doom.elf bin/doom"; \
		echo "rm bin/poll_test"; \
		echo "write userland/poll_test.elf bin/poll_test"; \
		echo "rm bin/pty_test"; \
		echo "write userland/pty_test.elf bin/pty_test"; \
		echo "rm bin/test_time"; \
		echo "write userland/test_time.elf bin/test_time"; \
		echo "rm bin/test_tsc_manual"; \
		echo "write userland/test_tsc_manual.elf bin/test_tsc_manual"; \
		echo "rm doom1.wad"; \
		echo "write doomu.wad doom1.wad"; \
		echo "rm bin/wget"; \
		echo "write userland/wget.elf bin/wget"; \
		echo "rm bin/test_unix_sock"; \
		echo "write userland/test_unix_sock.elf bin/test_unix_sock"; \
		echo "rm bin/test_unix_fdpass"; \
		echo "write userland/test_unix_fdpass.elf bin/test_unix_fdpass"; \
		echo "rm bin/test_fb"; \
		echo "write userland/test_fb.elf bin/test_fb"; \
		echo "rm bin/test_events"; \
		echo "write userland/test_events.elf bin/test_events"; \
		echo "rm bin/test_shared_irq"; \
		echo "write userland/test_shared_irq.elf bin/test_shared_irq"; \
		echo "rm bin/test_socket_phase3"; \
		echo "write userland/test_socket_phase3.elf bin/test_socket_phase3"; \
		echo "rm bin/test_socket_phase3_advanced"; \
		echo "write userland/test_socket_phase3_advanced.elf bin/test_socket_phase3_advanced"; \
		echo "rm bin/test_socket_phase3_megastress"; \
		echo "write userland/test_socket_phase3_megastress.elf bin/test_socket_phase3_megastress"; \
		echo "rm bin/test_socket_phase4"; \
		echo "write userland/test_socket_phase4.elf bin/test_socket_phase4"; \
		echo "rm bin/test_read"; \
		echo "write userland/test_read.elf bin/test_read"; \
		echo "rm bin/test_socket_phase4_detailed"; \
		echo "write userland/test_socket_phase4_detailed.elf bin/test_socket_phase4_detailed"; \
		echo "rm bin/test_socket_phase5"; \
		echo "write userland/test_socket_phase5.elf bin/test_socket_phase5"; \
		echo "rm bin/test_socket_phase6"; \
		echo "write userland/test_socket_phase6.elf bin/test_socket_phase6"; \
		echo "rm bin/test_socket_phase7"; \
		echo "write userland/test_socket_phase7.elf bin/test_socket_phase7"; \
		echo "rm bin/test_socket_phase7_advanced"; \
		echo "write userland/test_socket_phase7_advanced.elf bin/test_socket_phase7_advanced"; \
		echo "rm bin/test_socket_phase8"; \
		echo "write userland/test_socket_phase8.elf bin/test_socket_phase8"; \
		echo "rm bin/test_socket_phase9"; \
		echo "write userland/test_socket_phase9.elf bin/test_socket_phase9"; \
		echo "rm bin/test_socket_phase10"; \
		echo "write userland/test_socket_phase10.elf bin/test_socket_phase10"; \
		echo "rm bin/test_socket_phase11"; \
		echo "write userland/test_socket_phase11.elf bin/test_socket_phase11"; \
		echo "rm bin/test_inet_stress"; \
		echo "write userland/test_inet_stress.elf bin/test_inet_stress"; \
		echo "rm bin/dns_lookup"; \
		echo "write userland/dns_lookup.elf bin/dns_lookup"; \
		echo "rm bin/test_signal_subsystem"; \
		echo "write userland/test_signal_subsystem.elf bin/test_signal_subsystem"; \
		echo "rm test.tar"; \
		echo "write assets/test.tar test.tar"; \
		echo "rm bin/tglgears"; \
		echo "write userland/tglgears_fb.elf bin/tglgears"; \
		echo "rm bin/tglgears_drm"; \
		echo "write userland/tglgears_drm.elf bin/tglgears_drm"; \
		echo "rm bin/tglhello_drm"; \
		echo "write userland/tglhello_drm.elf bin/tglhello_drm"; \
		echo "rm bin/drm_bench"; \
		echo "write userland/drm_bench.elf bin/drm_bench"; \
		echo "rm bin/test_drm_kms"; \
		echo "write userland/test_drm_kms.elf bin/test_drm_kms"; \
		echo "rm bin/test_drm_flip"; \
		echo "write userland/test_drm_flip.elf bin/test_drm_flip"; \
		echo "rm bin/test_drm_atomic"; \
		echo "write userland/test_drm_atomic.elf bin/test_drm_atomic"; \
		echo "rm bin/test_drm_epoll"; \
		echo "write userland/test_drm_epoll.elf bin/test_drm_epoll"; \
		echo "rm bin/test_clone_futex"; \
		echo "write userland/test_clone_futex.elf bin/test_clone_futex"; \
		echo "rm bin/test_clone_futex_stress"; \
		echo "write userland/test_clone_futex_stress.elf bin/test_clone_futex_stress"; \
		echo "rm bin/test_mem_stress"; \
		echo "write userland/test_mem_stress.elf bin/test_mem_stress"; \
		echo "rm bin/test_io_leak"; \
		echo "write userland/test_io_leak.elf bin/test_io_leak"; \
		echo "rm bin/classicube"; \
		echo "write userland/classicube.elf bin/classicube"; \
		echo "mkdir texpacks"; \
		echo "rm terrain.png"; \
		echo "write userland/terrain.png terrain.png"; \
		echo "rm texpacks/classicube.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/classicube.zip"; \
		echo "rm texpacks/default.zip"; \
		echo "write userland/texpacks/classicube.zip texpacks/default.zip"; \
	} | debugfs -w ./part.img >/dev/null 2>&1 || true
	@echo "Writing ClassiCube options.txt (texture pack config)..."
	@printf 'texture-pack=classicube.zip\nskin-server=\n' > /tmp/classicube_options.txt
	debugfs -w -R "rm options.txt" ./part.img >/dev/null 2>&1 || true
	debugfs -w -R "write /tmp/classicube_options.txt options.txt" ./part.img >/dev/null 2>&1 || true
	rm -f /tmp/classicube_options.txt
	rm -f /tmp/ascentos_hello.txt /tmp/ascentos_readme.txt
	@if [ -d build/alpine/rootfs ]; then \
		echo "Populating Alpine Linux rootfs into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img build/alpine/rootfs /; \
	fi
	@echo "Populating root filesystem with additional tools..."

	@if [ -d toolchain/musl-sysroot/opt/tcc ]; then \
		echo "Installing TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/tcc opt/tcc; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/bin/tcc bin/tcc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/libc.a libc.a" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crt1.o crt1.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crti.o crti.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crtn.o crtn.o" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/lib/tcc/libtcc1.a libtcc1.a" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -d toolchain/musl-sysroot/opt/coreutils ]; then \
		echo "Installing coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/coreutils opt/coreutils; \
	fi
	@if [ -d toolchain/musl-sysroot/opt/bash ]; then \
		echo "Installing bash into disk image..."; \
		debugfs -w -R "mkdir opt" ./part.img >/dev/null 2>&1 || true; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/opt/bash opt/bash; \
		debugfs -w -R "rm bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/bash/bin/bash bin/bash" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm bin/sh" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/bash/bin/bash bin/sh" ./part.img >/dev/null 2>&1 || true; \
		echo "root:x:0:0:root:/root:/bin/bash" > /tmp/passwd; \
		echo "PS1='\033[0;32mRoot@AscentOS\033[0m:\w\\$$ '" > /tmp/bashrc; \
		echo "PATH=/usr/bin:/usr/local/bin:/opt/coreutils/bin:/bin:/opt/bash/bin:/opt/tcc/bin" >> /tmp/bashrc; \
		echo "HOME=/root" >> /tmp/bashrc; \
		echo "TERM=vt100" >> /tmp/bashrc; \
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
		debugfs -w -R "rm etc/passwd" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/passwd etc/passwd" ./part.img >/dev/null 2>&1 || true; \
		echo "NAME=\"AscentOS\"" > /tmp/os-release; \
		echo "PRETTY_NAME=\"AscentOS 0.1.0-alpha x86_64\"" >> /tmp/os-release; \
		echo "ID=ascentos" >> /tmp/os-release; \
		echo "VERSION_ID=0.1.0" >> /tmp/os-release; \
		echo "HOME_URL=\"https://github.com/AscentOS\"" >> /tmp/os-release; \
		debugfs -w -R "rm etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/os-release etc/os-release" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir root" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/.bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/bashrc root/.bashrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir root/.config" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir root/.config/fastfetch" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir root/fastfetch" ./part.img >/dev/null 2>&1 || true; \
		echo '{"logo": {"source": "/root/fastfetch/logo.txt", "type": "auto"}, "modules": ["title", "separator", "os", "host", "kernel", "uptime", "packages", "shell", "display", "de", "wm", "wmtheme", "theme", "icons", "font", "cursor", "terminal", "terminalfont", "cpu", "gpu", "memory", "swap", "disk", "battery", "poweradapter", "locale", "break", "colors"]}' > /tmp/ff_config.jsonc; \
		debugfs -w -R "rm root/.config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc root/.config/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write /tmp/ff_config.jsonc root/fastfetch/config.jsonc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write assets/ascii-art.txt root/fastfetch/logo.txt" ./part.img >/dev/null 2>&1 || true; \
		rm -f /tmp/passwd /tmp/bashrc /tmp/resolv.conf /tmp/hosts /tmp/ff_config.jsonc /tmp/os-release; \
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
	@if [ -f userland/Xfbdev.elf ]; then \
		echo "Installing updated Xfbdev server into disk image..."; \
		debugfs -w -R "rm bin/Xfbdev" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/Xfbdev.elf bin/Xfbdev" ./part.img >/dev/null 2>&1 || true; \
	elif [ -f toolchain/musl-sysroot/bin/Xfbdev ]; then \
		echo "Installing X11 server into disk image..."; \
		debugfs -w -R "rm bin/Xfbdev" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write toolchain/musl-sysroot/bin/Xfbdev bin/Xfbdev" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/xkbcomp.elf ]; then \
		echo "Installing xkbcomp into disk image..."; \
		debugfs -w -R "rm bin/xkbcomp" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/xkbcomp.elf bin/xkbcomp" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f toolchain/musl-sysroot/bin/Xfbdev ] || [ -f userland/Xfbdev.elf ]; then \
		echo "Installing XKB keyboard data..."; \
		debugfs -w -R "mkdir share" ./part.img 2>/dev/null || true; \
		debugfs -w -R "mkdir share/X11" ./part.img 2>/dev/null || true; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/share/X11/xkb share/X11/xkb; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/share/X11/locale share/X11/locale; \
		debugfs -w -R "write toolchain/musl-sysroot/share/X11/XErrorDB share/X11/XErrorDB" ./part.img; \
		debugfs -w -R "write toolchain/musl-sysroot/share/X11/Xcms.txt share/X11/Xcms.txt" ./part.img; \
		debugfs -w -R "mkdir share/X11/app-defaults" ./part.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home" ./part.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito" ./part.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito/AscentOS" ./part.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito/AscentOS/toolchain" ./part.img 2>/dev/null || true; \
		debugfs -w -R "symlink home/offihito/AscentOS/toolchain/musl-sysroot /" ./part.img; \
		./scripts/populate-ext2-dir.sh ./part.img toolchain/musl-sysroot/share/fonts share/fonts; \
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
		debugfs -w -R "mkdir root" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/.jwmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/jwmrc root/.jwmrc" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/bg.png" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write assets/room.png root/bg.png" ./part.img >/dev/null 2>&1 || true; \
	fi
	@if [ -f userland/doom_x11.elf ]; then \
		echo "Installing doom_x11 into disk image..."; \
		debugfs -w -R "rm bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/doom_x11.elf bin/doom_x11" ./part.img >/dev/null 2>&1 || true; \
	fi

	@if [ -f userland/xeyes.elf ] || [ -f userland/twm.elf ]; then \
		debugfs -w -R "rm bin/test_x11_simple" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write userland/test_x11_simple.elf bin/test_x11_simple" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "mkdir root" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "rm root/.Xauthority" ./part.img >/dev/null 2>&1 || true; \
		debugfs -w -R "write initrd/.Xauthority root/.Xauthority" ./part.img >/dev/null 2>&1 || true; \
	fi
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
clean-all: clean-musl clean-doom clean-coreutils clean-wolfssl clean-tar
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd nvme.img build/alpine

.PHONY: clean-coreutils
clean-coreutils:
	rm -rf build/coreutils-9.5
	rm -rf toolchain/musl-sysroot/opt/coreutils

.PHONY: clean-tar
clean-tar:
	rm -rf build/tar-1.35
	rm -f toolchain/musl-sysroot/bin/tar

.PHONY: clean-wolfssl
clean-wolfssl:
	rm -rf build/wolfssl-5.7.0
	rm -f $(WOLFSSL_LIB)
	rm -rf toolchain/musl-sysroot/include/wolfssl

.PHONY: clean-musl
clean-musl:
	rm -rf build/musl-1.2.5 build/musl-cross-make
	rm -rf toolchain/musl-sysroot toolchain/x86_64-linux-musl
	rm -f userland/hello.elf userland/test_syscalls.elf userland/test_kilo_syscalls.elf userland/test_kilo_asm.elf userland/kilo.elf userland/test_args.elf userland/kilo.c userland/test_mmap_shared_private.elf userland/playwav.elf userland/kria.elf userland/test_uname_pipe.elf userland/test_pipe_fork.elf userland/test_sys_access.elf userland/test_sys_cwd.elf userland/test_newfstatat.elf userland/ls.elf userland/readelf.elf userland/test_ext3.elf userland/poll_test.elf
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

userland/hello.elf: userland/hello.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/hello.c -o userland/hello.elf

userland/test_cpp.elf: userland/test_cpp.cpp $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CXX) $(MUSL_USER_CXXFLAGS) \
		userland/test_cpp.cpp -o userland/test_cpp.elf

userland/test_cow.elf: userland/test_cow.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_cow.c -o userland/test_cow.elf


userland/test_syscalls.elf: userland/test_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscalls.c -o userland/test_syscalls.elf


userland/test_kilo_syscalls.elf: userland/test_kilo_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_kilo_syscalls.c -o userland/test_kilo_syscalls.elf


userland/kilo.c:
	curl -L https://raw.githubusercontent.com/antirez/kilo/master/kilo.c -o userland/kilo.c

userland/kilo.elf: userland/kilo.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/kilo.c -o userland/kilo.elf

userland/test_args.elf: userland/test_args.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_args.c -o userland/test_args.elf

userland/test_stat.elf: userland/test_stat.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_stat.c -o userland/test_stat.elf

userland/test_mmap_shared_private.elf: userland/test_mmap_shared_private.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mmap_shared_private.c -o userland/test_mmap_shared_private.elf

userland/ls.elf: userland/ls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ls.c -o userland/ls.elf

userland/readelf.elf: userland/readelf.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/readelf.c -o userland/readelf.elf

userland/pong.elf: userland/pong.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/pong.c -o userland/pong.elf

userland/raycast.elf: userland/raycast.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/raycast.c -lm -o userland/raycast.elf

userland/playwav.elf: userland/playwav.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/playwav.c -o userland/playwav.elf

userland/test_ext3.elf: userland/test_ext3.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_ext3.c -o userland/test_ext3.elf

userland/showbmp.elf: userland/showbmp.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/showbmp.c -o userland/showbmp.elf

userland/test_uname_pipe.elf: userland/test_uname_pipe.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_uname_pipe.c -o userland/test_uname_pipe.elf

userland/test_pipe_fork.elf: userland/test_pipe_fork.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_pipe_fork.c -o userland/test_pipe_fork.elf

userland/test_socket_phase3.elf: userland/test_socket_phase3.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3.c -o userland/test_socket_phase3.elf

userland/test_socket_phase3_advanced.elf: userland/test_socket_phase3_advanced.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3_advanced.c -o userland/test_socket_phase3_advanced.elf

userland/test_socket_phase3_megastress.elf: userland/test_socket_phase3_megastress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3_megastress.c -o userland/test_socket_phase3_megastress.elf

userland/test_socket_phase4.elf: userland/test_socket_phase4.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase4.c -o userland/test_socket_phase4.elf

userland/test_socket_phase4_detailed.elf: userland/test_socket_phase4_detailed.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase4_detailed.c -o userland/test_socket_phase4_detailed.elf

userland/test_socket_phase5.elf: userland/test_socket_phase5.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase5.c -o userland/test_socket_phase5.elf

userland/test_socket_phase6.elf: userland/test_socket_phase6.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase6.c -o userland/test_socket_phase6.elf

userland/test_socket_phase7.elf: userland/test_socket_phase7.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase7.c -o userland/test_socket_phase7.elf

userland/test_socket_phase7_advanced.elf: userland/test_socket_phase7_advanced.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase7_advanced.c -o userland/test_socket_phase7_advanced.elf

userland/test_socket_phase8.elf: userland/test_socket_phase8.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase8.c -o userland/test_socket_phase8.elf

userland/test_socket_phase9.elf: userland/test_socket_phase9.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase9.c -o userland/test_socket_phase9.elf

userland/test_socket_phase10.elf: userland/test_socket_phase10.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase10.c -o userland/test_socket_phase10.elf

userland/test_socket_phase11.elf: userland/test_socket_phase11.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase11.c -o userland/test_socket_phase11.elf

userland/test_inet_stress.elf: userland/test_inet_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_inet_stress.c -o userland/test_inet_stress.elf

userland/test_sys_access.elf: userland/test_sys_access.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_sys_access.c -o userland/test_sys_access.elf

userland/test_sys_cwd.elf: userland/test_sys_cwd.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_sys_cwd.c -o userland/test_sys_cwd.elf

userland/test_newfstatat.elf: userland/test_newfstatat.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_newfstatat.c -o userland/test_newfstatat.elf

userland/test_unlink_rename.elf: userland/test_unlink_rename.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unlink_rename.c -o userland/test_unlink_rename.elf

userland/test_dup.elf: userland/test_dup.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_dup.c -o userland/test_dup.elf

userland/test_attrib.elf: userland/test_attrib.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_attrib.c -o userland/test_attrib.elf

userland/test_symlink.elf: userland/test_symlink.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_symlink.c -o userland/test_symlink.elf

userland/test_cred.elf: userland/test_cred.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_cred.c -o userland/test_cred.elf

userland/test_wait4_complex.elf: userland/test_wait4_complex.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_wait4_complex.c -o userland/test_wait4_complex.elf

userland/test_dynamic.elf: userland/test_dynamic.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) -O2 -Wall -Wextra -fno-stack-protector -I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib \
		userland/test_dynamic.c -o userland/test_dynamic.elf

userland/poll_test.elf: userland/poll_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/poll_test.c -o userland/poll_test.elf

userland/test_time.elf: userland/test_time.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_time.c -o userland/test_time.elf

userland/test_tsc_manual.elf: userland/test_tsc_manual.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_tsc_manual.c -o userland/test_tsc_manual.elf

userland/test_unix_sock.elf: userland/test_unix_sock.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unix_sock.c -o userland/test_unix_sock.elf

$(WOLFSSL_LIB): $(MUSL_LIBC)
	./scripts/build-wolfssl.sh

.PHONY: wolfssl
wolfssl: $(WOLFSSL_LIB)

userland/wget.elf: $(MUSL_LIBC) $(WOLFSSL_LIB)
	./scripts/build-wget.sh


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

test.wav:
	python3 gen_wav.py

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

userland/test_unix_fdpass.elf: userland/test_unix_fdpass.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unix_fdpass.c -o userland/test_unix_fdpass.elf

userland/test_fb.elf: userland/test_fb.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_fb.c -o userland/test_fb.elf

userland/test_events.elf: userland/test_events.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_events.c -o userland/test_events.elf

userland/test_read.elf: userland/test_read.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_read.c -o userland/test_read.elf

userland/test_x11_simple.elf: userland/test_x11_simple.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_x11_simple.c -L$(MUSL_SYSROOT)/lib -lX11 -lxcb -lXau -lXdmcp -o userland/test_x11_simple.elf

userland/xkbcomp.elf: scripts/port-x11.sh
	./scripts/port-x11.sh

userland/test_shared_irq.elf: userland/test_shared_irq.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_shared_irq.c -o userland/test_shared_irq.elf

userland/pty_test.elf: userland/pty_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/pty_test.c -o userland/pty_test.elf

userland/gtk_test.elf: userland/gtk_test.c scripts/build-gtktest.sh scripts/setup-alpine.sh
	./scripts/build-gtktest.sh

userland/tinywl.elf: userland/tinywl.c scripts/build-tinywl.sh scripts/setup-alpine.sh
	chmod +x scripts/build-tinywl.sh
	./scripts/build-tinywl.sh

userland/tglgears_fb.elf: userland/tglgears_fb.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_fb.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -L$(MUSL_SYSROOT)/lib -lX11 -lxcb -lXau -lXdmcp -lm -o userland/tglgears_fb.elf

userland/test_clone_futex.elf: userland/test_clone_futex.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_clone_futex.c -o userland/test_clone_futex.elf

userland/test_clone_futex_stress.elf: userland/test_clone_futex_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_clone_futex_stress.c -o userland/test_clone_futex_stress.elf

userland/test_mem_stress.elf: userland/test_mem_stress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mem_stress.c -o userland/test_mem_stress.elf

userland/test_io_leak.elf: userland/test_io_leak.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_io_leak.c -o userland/test_io_leak.elf

userland/tglgears_drm.elf: userland/tglgears_drm.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglgears_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglgears_drm.elf

userland/tglhello_drm.elf: userland/tglhello_drm.c $(MUSL_LIBC) scripts/build-tinygl.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/tglhello_drm.c -I$(MUSL_SYSROOT)/opt/tinygl/include -L$(MUSL_SYSROOT)/opt/tinygl/lib -lTinyGL -lm -o userland/tglhello_drm.elf

userland/drm_bench.elf: userland/drm_bench.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/drm_bench.c -o userland/drm_bench.elf

userland/test_drm_kms.elf: userland/test_drm_kms.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_drm_kms.c -o userland/test_drm_kms.elf

userland/test_drm_flip.elf: userland/test_drm_flip.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_drm_flip.c -o userland/test_drm_flip.elf

userland/test_drm_atomic.elf: userland/test_drm_atomic.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_drm_atomic.c -o userland/test_drm_atomic.elf

userland/test_drm_epoll.elf: userland/test_drm_epoll.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_drm_epoll.c -o userland/test_drm_epoll.elf

userland/dns_lookup.elf: userland/dns_lookup.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/dns_lookup.c -o userland/dns_lookup.elf

userland/test_signal_subsystem.elf: userland/test_signal_subsystem.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_signal_subsystem.c -o userland/test_signal_subsystem.elf

userland/classicube.elf: scripts/build-classicube.sh
	./scripts/build-classicube.sh

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
