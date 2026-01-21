#!/bin/bash
set -e

KARCH="x86_64"
OVMF_DIR="ovmf"
DISK_DIR="disks"
IMAGE_NAME="eucalypt-${KARCH}"
QEMUFLAGS="-m 2G"
ISO_ROOT="iso_root"
LIMINE_DIR="limine"

build_kernel() {
    echo "Building eucalyptOS kernel..."
    make -C kernel
}

setup_limine() {
    if [ ! -d "${LIMINE_DIR}" ]; then
        echo "Cloning Limine bootloader..."
        git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
    fi
}

build_iso() {
    echo "Building ISO image..."
    
    setup_limine
    
    rm -rf "${ISO_ROOT}"
    mkdir -p "${ISO_ROOT}"
    
    if [ ! -f "kernel/kernel" ]; then
        echo "ERROR: Kernel binary not found at kernel/kernel"
        exit 1
    fi
    cp kernel/kernel "${ISO_ROOT}/"
    
    mkdir -p "${ISO_ROOT}/boot"
    cp "${LIMINE_DIR}/limine-bios.sys" "${ISO_ROOT}/boot/" 2>/dev/null || true
    cp "${LIMINE_DIR}/limine-bios-cd.bin" "${ISO_ROOT}/boot/" 2>/dev/null || true
    cp "${LIMINE_DIR}/limine-uefi-cd.bin" "${ISO_ROOT}/boot/" 2>/dev/null || true
    
    mkdir -p "${ISO_ROOT}/EFI/BOOT"
    cp "${LIMINE_DIR}/BOOTX64.EFI" "${ISO_ROOT}/EFI/BOOT/" 2>/dev/null || true
    cp "${LIMINE_DIR}/BOOTIA32.EFI" "${ISO_ROOT}/EFI/BOOT/" 2>/dev/null || true
    
    cat > "${ISO_ROOT}/boot/limine.conf" << 'EOF'
timeout: 0

/eucalyptOS
    protocol: limine
    kernel_path: boot():/kernel
EOF
    
    echo "Creating ISO with xorriso..."
    xorriso -as mkisofs \
        -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "${ISO_ROOT}" -o "${IMAGE_NAME}.iso" 2>/dev/null
    
    if [ -f "${LIMINE_DIR}/limine" ]; then
        "${LIMINE_DIR}/limine" bios-install "${IMAGE_NAME}.iso" 2>/dev/null || true
    fi
    
    if [ ! -f "${IMAGE_NAME}.iso" ]; then
        echo "ERROR: ISO file ${IMAGE_NAME}.iso was not created!"
        exit 1
    fi
    
    echo "✓ ISO created successfully: ${IMAGE_NAME}.iso ($(du -h ${IMAGE_NAME}.iso | cut -f1))"
}

create_disks() {
    echo "Creating disk images..."
    mkdir -p "${DISK_DIR}"
    
    if [ ! -f "${DISK_DIR}/ide_disk.img" ]; then
        echo "  Creating IDE disk (64MB)..."
        dd if=/dev/zero of="${DISK_DIR}/ide_disk.img" bs=1M count=64 status=none
    fi
    
    if [ ! -f "${DISK_DIR}/ahci_disk.img" ]; then
        echo "  Creating AHCI disk (64MB)..."
        dd if=/dev/zero of="${DISK_DIR}/ahci_disk.img" bs=1M count=64 status=none
    fi
    
    echo "✓ Disk images ready"
}

run_qemu() {
    echo "Starting QEMU..."
    
    if [ ! -f "${IMAGE_NAME}.iso" ]; then
        echo "ERROR: ${IMAGE_NAME}.iso not found!"
        exit 1
    fi
    
    if [ ! -f "${OVMF_DIR}/ovmf-code-${KARCH}.fd" ]; then
        echo "ERROR: OVMF firmware not found at ${OVMF_DIR}/ovmf-code-${KARCH}.fd"
        echo "You may need to install ovmf package or copy firmware files"
        exit 1
    fi
    
    # Run QEMU with clean environment to avoid snap library conflicts
    env -i \
        PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
        HOME="$HOME" \
        DISPLAY="$DISPLAY" \
        XAUTHORITY="$XAUTHORITY" \
        TERM="$TERM" \
        USER="$USER" \
        qemu-system-${KARCH} \
        -M pc \
        -drive if=pflash,unit=0,format=raw,file=${OVMF_DIR}/ovmf-code-${KARCH}.fd,readonly=on \
        -drive if=pflash,unit=1,format=raw,file=${OVMF_DIR}/ovmf-vars-${KARCH}.fd \
        -cdrom ${IMAGE_NAME}.iso \
        -drive file=${DISK_DIR}/ide_disk.img,format=raw,if=ide,index=0,media=disk \
        -drive file=${DISK_DIR}/ahci_disk.img,format=raw,if=none,id=ahci0 \
        -device ahci,id=ahci \
        -device ide-hd,drive=ahci0,bus=ahci.0 \
        ${QEMUFLAGS}
}

clean() {
    echo "Cleaning build artifacts..."
    make -C kernel clean
    rm -rf "${ISO_ROOT}"
    rm -f "${IMAGE_NAME}.iso"
    echo "✓ Clean complete"
}

distclean() {
    echo "Deep cleaning..."
    clean
    rm -rf "${LIMINE_DIR}"
    rm -rf "${DISK_DIR}"
    echo "✓ Distclean complete"
}

case "${1:-}" in
    build)
        build_kernel
        build_iso
        ;;
    run)
        build_kernel
        build_iso
        create_disks
        run_qemu
        ;;
    clean)
        clean
        ;;
    distclean)
        clean
        ;;
    kernel)
        build_kernel
        ;;
    iso)
        build_iso
        ;;
    *)
        echo "Usage: $0 {build|run|clean|distclean|kernel|iso}"
        echo "  build     - Build kernel and ISO"
        echo "  run       - Build and run in QEMU (default)"
        echo "  kernel    - Build kernel only"
        echo "  iso       - Build ISO only (requires kernel)"
        echo "  clean     - Clean build artifacts"
        echo "  distclean - Deep clean everything"
        exit 1
        ;;
esac