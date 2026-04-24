MAKEFLAGS += -rR
.SUFFIXES:

override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

$(call USER_VARIABLE,KARCH,x86_64)
$(call USER_VARIABLE,QEMUFLAGS,-m 510M)

override IMAGE_NAME := eucalypt-$(KARCH)

CC      := x86_64-linux-gnu-gcc
OBJCOPY := x86_64-linux-gnu-objcopy

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: run-$(KARCH)

.PHONY: run-hdd
run-hdd: run-hdd-$(KARCH)

.PHONY: run-x86_64
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso
	qemu-system-$(KARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(KARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-drive file=disks/ide_disk.img,format=raw,if=ide,index=0,media=disk \
		-drive file=disks/ahci_disk.img,format=raw,if=none,id=ahci0 \
		-device ahci,id=ahci \
		-device ide-hd,drive=ahci0,bus=ahci.0 \
		-smp 4 \
		-m 10G \

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso
	qemu-system-$(KARCH) -M q35 -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS)

.PHONY: run-hdd-bios
run-hdd-bios: $(IMAGE_NAME).hdd
	qemu-system-$(KARCH) -M q35 -hda $(IMAGE_NAME).hdd $(QEMUFLAGS)

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=v11.x-binary --depth=1
	$(MAKE) -C limine

.PHONY: kernel
kernel:
	$(MAKE) -C kernel

.PHONY: userspace
userspace: test_userspace/INIT

test_userspace/INIT: test_userspace/main.c test_userspace/user.ld
	$(CC) -ffreestanding -nostdlib -static -m64 -T test_userspace/user.ld -o $@ $< -lgcc

.PHONY: disks
disks: userspace
	mkdir -p disks
	mkdir -p z_files_to_copy
	cp test_userspace/INIT z_files_to_copy/INIT
	# Create 4MB ram.img
	rm -f disks/ram.img
	mkfs.fat -C disks/ram.img 4096
	mmd -i disks/ram.img ::/bin
	mcopy -i disks/ram.img z_files_to_copy/INIT ::/INIT
	# Create 32MB disk images
	rm -f disks/ide_disk.img disks/ahci_disk.img
	mkfs.fat -C disks/ide_disk.img 32768
	mkfs.fat -C disks/ahci_disk.img 32768

$(IMAGE_NAME).iso: limine/limine kernel disks
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/mod iso_root/EFI/BOOT
	cp -v kernel/kernel iso_root/boot/
	cp -v disks/ram.img iso_root/mod/ramfs.img
	cp -v limine.conf iso_root/boot/limine/
ifeq ($(KARCH),x86_64)
	cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine/BOOTX64.EFI limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $@
	./limine/limine bios-install $@
else
	cp -v limine/limine-uefi-cd.bin iso_root/boot/limine/
	$(if $(filter aarch64,$(KARCH)),cp limine/BOOTAA64.EFI iso_root/EFI/BOOT/,)
	$(if $(filter riscv64,$(KARCH)),cp limine/BOOTRISCV64.EFI iso_root/EFI/BOOT/,)
	$(if $(filter loongarch64,$(KARCH)),cp limine/BOOTLOONGARCH64.EFI iso_root/EFI/BOOT/,)
	xorriso -as mkisofs --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $@
endif
	rm -rf iso_root

$(IMAGE_NAME).hdd: limine/limine kernel disks
	rm -f $@
	dd if=/dev/zero bs=1M count=0 seek=64 of=$@
	sgdisk $@ -n 1:2048 -t 1:ef00
ifeq ($(KARCH),x86_64)
	./limine/limine bios-install $@
endif
	mformat -i $@@@1M
	mmd -i $@@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@@@1M kernel/kernel ::/boot
	mcopy -i $@@@1M limine.conf ::/boot/limine
ifeq ($(KARCH),x86_64)
	mcopy -i $@@@1M limine/limine-bios.sys ::/boot/limine
	mcopy -i $@@@1M limine/BOOTX64.EFI limine/BOOTIA32.EFI ::/EFI/BOOT
else ifeq ($(KARCH),aarch64)
	mcopy -i $@@@1M limine/BOOTAA64.EFI ::/EFI/BOOT
else ifeq ($(KARCH),riscv64)
	mcopy -i $@@@1M limine/BOOTRISCV64.EFI ::/EFI/BOOT
else ifeq ($(KARCH),loongarch64)
	mcopy -i $@@@1M limine/BOOTLOONGARCH64.EFI ::/EFI/BOOT
endif

.PHONY: clean distclean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd test_userspace/INIT z_files_to_copy disks/*.img

distclean: clean
	$(MAKE) -C kernel distclean
	rm -rf limine edk2-ovmf disks
