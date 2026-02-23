#!/bin/bash
set -e

KARCH="x86_64"
OVMF_DIR="ovmf"
DISK_DIR="disks"
IMAGE_NAME="eucalypt-${KARCH}"
QEMUFLAGS="-m 2G"
ISO_ROOT="iso_root"
LIMINE_DIR="limine"
FILES_TO_COPY="z_files_to_copy"

build_kernel() {
    echo "Building eucalyptOS kernel..."
    make -C kernel
}

setup_limine() {
    if [ ! -d "${LIMINE_DIR}" ]; then
        echo "Cloning Limine bootloader..."
        git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
    fi
    
    echo "Checking Limine files..."
    ls -la "${LIMINE_DIR}/" || true
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
    cp -v kernel/kernel "${ISO_ROOT}/"
    
    mkdir -p "${ISO_ROOT}/boot"
    
    echo "Copying Limine boot files..."
    if [ -f "${LIMINE_DIR}/limine-bios.sys" ]; then
        cp -v "${LIMINE_DIR}/limine-bios.sys" "${ISO_ROOT}/boot/"
    else
        echo "WARNING: limine-bios.sys not found"
    fi
    
    if [ -f "${LIMINE_DIR}/limine-bios-cd.bin" ]; then
        cp -v "${LIMINE_DIR}/limine-bios-cd.bin" "${ISO_ROOT}/boot/"
    else
        echo "ERROR: limine-bios-cd.bin not found - this is required!"
        ls -la "${LIMINE_DIR}/" | grep -i limine
        exit 1
    fi
    
    if [ -f "${LIMINE_DIR}/limine-uefi-cd.bin" ]; then
        cp -v "${LIMINE_DIR}/limine-uefi-cd.bin" "${ISO_ROOT}/boot/"
    else
        echo "WARNING: limine-uefi-cd.bin not found"
    fi
    
    mkdir -p "${ISO_ROOT}/EFI/BOOT"
    if [ -f "${LIMINE_DIR}/BOOTX64.EFI" ]; then
        cp -v "${LIMINE_DIR}/BOOTX64.EFI" "${ISO_ROOT}/EFI/BOOT/"
    else
        echo "WARNING: BOOTX64.EFI not found"
    fi
    
    if [ -f "${LIMINE_DIR}/BOOTIA32.EFI" ]; then
        cp -v "${LIMINE_DIR}/BOOTIA32.EFI" "${ISO_ROOT}/EFI/BOOT/"
    else
        echo "WARNING: BOOTIA32.EFI not found"
    fi
    
    echo "Creating limine.conf..."
    cat > "${ISO_ROOT}/boot/limine.conf" << 'EOF'
timeout: 0

/eucalyptOS
    protocol: limine
    kernel_path: boot():/kernel
EOF
    
    echo "ISO root contents:"
    find "${ISO_ROOT}" -type f
    
    echo "Creating ISO with xorriso..."
    if ! command -v xorriso &> /dev/null; then
        echo "ERROR: xorriso not found! Install with: sudo apt install xorriso"
        exit 1
    fi
    
    xorriso -as mkisofs \
        -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "${ISO_ROOT}" -o "${IMAGE_NAME}.iso"
    
    if [ -f "${LIMINE_DIR}/limine" ] || [ -f "${LIMINE_DIR}/limine-deploy" ]; then
        echo "Installing Limine bootloader to ISO..."
        if [ -f "${LIMINE_DIR}/limine" ]; then
            "${LIMINE_DIR}/limine" bios-install "${IMAGE_NAME}.iso" || echo "Warning: limine bios-install failed"
        elif [ -f "${LIMINE_DIR}/limine-deploy" ]; then
            "${LIMINE_DIR}/limine-deploy" "${IMAGE_NAME}.iso" || echo "Warning: limine-deploy failed"
        fi
    else
        echo "WARNING: Limine installer not found"
    fi
    
    if [ ! -f "${IMAGE_NAME}.iso" ]; then
        echo "ERROR: ISO file ${IMAGE_NAME}.iso was not created!"
        exit 1
    fi
    
    echo "✓ ISO created successfully: ${IMAGE_NAME}.iso ($(du -h ${IMAGE_NAME}.iso | cut -f1))"
}

format_fat12() {
    local disk_file="$1"
    local size_mb="$2"

    echo "  Formatting ${disk_file} as FAT12..."

    if ! command -v mkfs.fat &> /dev/null; then
        echo "ERROR: mkfs.fat not found! Install with: sudo apt install dosfstools"
        exit 1
    fi

    if ! command -v mcopy &> /dev/null; then
        echo "ERROR: mcopy not found! Install with: sudo apt install mtools"
        exit 1
    fi

    dd if=/dev/zero of="${disk_file}" bs=1M count=${size_mb} status=none
    mkfs.fat -F 12 -n "EUCALYPT" "${disk_file}" > /dev/null 2>&1
    echo "  ✓ FAT12 filesystem created on ${disk_file}"

    if [ -d "${FILES_TO_COPY}" ] && [ -n "$(ls -A ${FILES_TO_COPY} 2>/dev/null)" ]; then
        echo "  Copying files from ${FILES_TO_COPY}/ into ${disk_file}..."
        for file in "${FILES_TO_COPY}"/*; do
            if [ -f "${file}" ]; then
                echo "    Copying $(basename ${file})..."
                mcopy -i "${disk_file}" "${file}" "::/$(basename ${file})"
            fi
        done
        echo "  ✓ Files copied successfully"
    else
        echo "  No files to copy (${FILES_TO_COPY}/ is empty or missing)"
    fi
}

create_disks() {
    echo "Creating disk images..."
    mkdir -p "${DISK_DIR}"
    
    echo "  Creating IDE disk (64MB) with FAT12..."
    format_fat12 "${DISK_DIR}/ide_disk.img" 64
    
    echo "  Creating AHCI disk (64MB)..."
    dd if=/dev/zero of="${DISK_DIR}/ahci_disk.img" bs=1M count=64 status=none
    
    echo "✓ Disk images ready"
}

run_qemu() {    
    if [ ! -f "${IMAGE_NAME}.iso" ]; then
        echo "ERROR: ${IMAGE_NAME}.iso not found!"
        exit 1
    fi
    
    if [ ! -f "${OVMF_DIR}/ovmf-code-${KARCH}.fd" ]; then
        echo "ERROR: OVMF firmware not found at ${OVMF_DIR}/ovmf-code-${KARCH}.fd"
        echo "You may need to install ovmf package or copy firmware files"
        exit 1
    fi
    echo "Starting QEMU"
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
        -smp 4 \
        ${QEMUFLAGS}
}

run_qemu_codespace() {
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
    echo "Starting QEMU"
    qemu-system-${KARCH} \
    -M pc -display curses\
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
    run-codespace)
        build_kernel
        build_iso
        create_disks
        run_qemu_codespace
        ;;
    clean)
        clean
        ;;
    distclean)
        distclean
        ;;
    kernel)
        build_kernel
        ;;
    iso)
        build_iso
        ;;
    disks)
        create_disks
        ;;
    *)
        echo "Usage: $0 {build|run|clean|distclean|kernel|iso|disks}"
        echo "  build        - Build kernel and ISO"
        echo "  run          - Build and run in QEMU (default)"
        echo "  run-codespace- Build and run in QEMU with codespaces"
        echo "  kernel       - Build kernel only"
        echo "  iso          - Build ISO only (requires kernel)"
        echo "  disks        - Create disk images only"
        echo "  clean        - Clean build artifacts"
        echo "  distclean    - Deep clean everything"
        exit 1
        ;;
esac