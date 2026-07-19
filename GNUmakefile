ARCH := x86_64
MAKE := make
QEMUFLAGS := -m 2G -cpu IvyBridge -netdev user,id=net1 -device rtl8139,netdev=net1,bus=pcie.0,addr=0x3 -smp 6 -d int
IMAGE_NAME := eucalypt-$(ARCH)
USERLAND_DIR := ../Eucalypt-Userland
HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=
ARCHIVE := main_archive.tar

.PHONY: all all-hdd run run-hdd kernel disks archive edk2-ovmf limine-binary \
        iso hdd run-x86_64 run-hdd-x86_64 run-bios run-hdd-bios clean distclean

.DEFAULT_GOAL := all

all: iso

all-hdd: hdd

run: run-x86_64

run-hdd: run-hdd-x86_64

kernel:
	test -f kernel/.deps-obtained || ./kernel/get-deps
	$(MAKE) -C kernel ARCH=$(ARCH)

disks:
	mkdir -p disks
	rm -f disks/ram.img
	rm -f disks/ide_disk.img disks/ahci_disk.img
	PATH=$$PATH:/usr/sbin:/sbin mkfs.fat -F 12 -C disks/ide_disk.img 32768
	PATH=$$PATH:/usr/sbin:/sbin mkfs.fat -F 12 -C disks/ahci_disk.img 32768

archive:
	tar --format=ustar -cf $(ARCHIVE) build/

edk2-ovmf:
	rm -rf edk2-ovmf
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine-binary:
	rm -rf limine-binary
	curl -L https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.gz | gunzip | tar -xf -
	$(MAKE) -C limine-binary CC=cc CFLAGS='-g -O2 -pipe' CPPFLAGS= LDFLAGS= LIBS=

iso: limine-binary kernel disks
	rm -rf iso_root
	mkdir -p iso_root/boot iso_root/boot/limine iso_root/EFI/BOOT iso_root/archive
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	cp -v limine.conf iso_root/boot/limine/
	cp -v $(ARCHIVE) iso_root/archive/main_archive.tar
	if [ $(ARCH) = x86_64 ]; then \
		cp -v limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin limine-binary/limine-uefi-cd.bin iso_root/boot/limine/ && \
		cp -v limine-binary/BOOTX64.EFI limine-binary/BOOTIA32.EFI iso_root/EFI/BOOT/ && \
		xorriso -as mkisofs -input-charset utf-8 -output-charset utf-8 -R -r -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
			--efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
			--protective-msdos-label iso_root -o $(IMAGE_NAME).iso && \
		./limine-binary/limine bios-install $(IMAGE_NAME).iso; \
	fi
	if [ $(ARCH) = aarch64 ]; then \
		cp -v limine-binary/limine-uefi-cd.bin iso_root/boot/limine/ && \
		cp -v limine-binary/BOOTAA64.EFI iso_root/EFI/BOOT/ && \
		xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
			-efi-boot-part --efi-boot-image --protective-msdos-label iso_root -o $(IMAGE_NAME).iso; \
	fi
	if [ $(ARCH) = riscv64 ]; then \
		cp -v limine-binary/limine-uefi-cd.bin iso_root/boot/limine/ && \
		cp -v limine-binary/BOOTRISCV64.EFI iso_root/EFI/BOOT/ && \
		xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
			-efi-boot-part --efi-boot-image --protective-msdos-label iso_root -o $(IMAGE_NAME).iso; \
	fi
	if [ $(ARCH) = loongarch64 ]; then \
		cp -v limine-binary/limine-uefi-cd.bin iso_root/boot/limine/ && \
		cp -v limine-binary/BOOTLOONGARCH64.EFI iso_root/EFI/BOOT/ && \
		xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
			-efi-boot-part --efi-boot-image --protective-msdos-label iso_root -o $(IMAGE_NAME).iso; \
	fi
	rm -rf iso_root

hdd: limine-binary kernel
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
	if [ $(ARCH) = x86_64 ]; then \
		PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00 -m 1 && \
		./limine-binary/limine bios-install $(IMAGE_NAME).hdd; \
	else \
		PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00; \
	fi
	mformat -i $(IMAGE_NAME).hdd@@1M
	mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine ::/archive
	mcopy -i $(IMAGE_NAME).hdd@@1M kernel/bin-$(ARCH)/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@1M $(ARCHIVE) ::/archive/main_archive.tar
	mcopy -i $(IMAGE_NAME).hdd@@1M limine.conf ::/boot/limine
	if [ $(ARCH) = x86_64 ]; then \
		mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/limine-bios.sys ::/boot/limine && \
		mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTX64.EFI limine-binary/BOOTIA32.EFI ::/EFI/BOOT; \
	fi
	if [ $(ARCH) = aarch64 ]; then \
		mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTAA64.EFI ::/EFI/BOOT; \
	fi
	if [ $(ARCH) = riscv64 ]; then \
		mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTRISCV64.EFI ::/EFI/BOOT; \
	fi
	if [ $(ARCH) = loongarch64 ]; then \
		mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTLOONGARCH64.EFI ::/EFI/BOOT; \
	fi

run-x86_64: edk2-ovmf iso
	qemu-system-x86_64 -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on -cdrom $(IMAGE_NAME).iso $(QEMUFLAGS)

run-hdd-x86_64: edk2-ovmf hdd
	qemu-system-x86_64 -M q35 -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on -hda $(IMAGE_NAME).hdd $(QEMUFLAGS)

run-bios: iso
	qemu-system-$(ARCH) -M q35 -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS)

run-hdd-bios: hdd
	qemu-system-$(ARCH) -M q35 -hda $(IMAGE_NAME).hdd $(QEMUFLAGS)

clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

distclean:
	$(MAKE) -C kernel distclean
	$(MAKE) -C archive clean
	rm -rf iso_root *.iso *.hdd limine-binary edk2-ovmf