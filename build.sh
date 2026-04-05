#!/bin/bash
set -e

KARCH="x86_64"
OVMF_DIR="ovmf"
DISK_DIR="disks"
IMAGE_NAME="eucalypt-${KARCH}"
ISO_ROOT="iso_root"
LIMINE_DIR="limine"

build_kernel() {
    make -C kernel
}

setup_limine() {
    if [ ! -d "${LIMINE_DIR}" ]; then
        git clone https://github.com/limine-bootloader/limine.git --branch=v11.x-binary --depth=1
    fi
}

build_iso() {
    setup_limine

    rm -rf "${ISO_ROOT}"
    mkdir -p "${ISO_ROOT}"

    if [ ! -f "kernel/kernel" ]; then
        echo "kernel missing"
        exit 1
    fi

    cp kernel/kernel "${ISO_ROOT}/"
    mkdir -p "${ISO_ROOT}/boot"
    mkdir -p "${ISO_ROOT}/mod"

    [ -f "${LIMINE_DIR}/limine-bios.sys" ] && cp "${LIMINE_DIR}/limine-bios.sys" "${ISO_ROOT}/boot/"
    [ -f "${LIMINE_DIR}/limine-bios-cd.bin" ] || exit 1
    cp "${LIMINE_DIR}/limine-bios-cd.bin" "${ISO_ROOT}/boot/"
    [ -f "${LIMINE_DIR}/limine-uefi-cd.bin" ] && cp "${LIMINE_DIR}/limine-uefi-cd.bin" "${ISO_ROOT}/boot/"

    mkdir -p "${ISO_ROOT}/EFI/BOOT"
    [ -f "${LIMINE_DIR}/BOOTX64.EFI" ] && cp "${LIMINE_DIR}/BOOTX64.EFI" "${ISO_ROOT}/EFI/BOOT/"
    [ -f "${LIMINE_DIR}/BOOTIA32.EFI" ] && cp "${LIMINE_DIR}/BOOTIA32.EFI" "${ISO_ROOT}/EFI/BOOT/"

    cp ./limine.conf ${ISO_ROOT}/boot/
    cp ./${DISK_DIR}/ramfs.img ${ISO_ROOT}/mod/

    command -v xorriso >/dev/null 2>&1 || exit 1

    xorriso -as mkisofs \
        -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "${ISO_ROOT}" -o "${IMAGE_NAME}.iso"

    if [ -f "${LIMINE_DIR}/limine" ]; then
        "${LIMINE_DIR}/limine" bios-install "${IMAGE_NAME}.iso" || true
    elif [ -f "${LIMINE_DIR}/limine-deploy" ]; then
        "${LIMINE_DIR}/limine-deploy" "${IMAGE_NAME}.iso" || true
    fi

    [ -f "${IMAGE_NAME}.iso" ] || exit 1

    echo "ISO ready: ${IMAGE_NAME}.iso"
}

format_fat12() {
    local disk_file="$1"
    local size_mb="$2"

    command -v mkfs.fat >/dev/null 2>&1 || exit 1

    dd if=/dev/zero of="${disk_file}" bs=1M count=${size_mb} status=none
    mkfs.fat -F 12 -n "EUCALYPT" "${disk_file}" >/dev/null 2>&1
}

create_disks() {
    mkdir -p "${DISK_DIR}"
    format_fat12 "${DISK_DIR}/ide_disk.img" 64
    format_fat12 "${DISK_DIR}/ahci_disk.img" 2
    format_fat12 "${DISK_DIR}/ramfs.img" 64
}

find_ovmf() {
    if [ -f "/usr/share/edk2/x64/OVMF_CODE.4m.fd" ]; then
        OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"
        OVMF_VARS_LOCAL="${OVMF_DIR}/ovmf-vars-${KARCH}.fd"

        mkdir -p "${OVMF_DIR}"

        if [ ! -f "${OVMF_VARS_LOCAL}" ]; then
            cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "${OVMF_VARS_LOCAL}"
        fi

        OVMF_VARS="${OVMF_VARS_LOCAL}"
        return
    fi

    if [ -f "/usr/share/edk2/x64/OVMF_CODE.fd" ]; then
        OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.fd"
        OVMF_VARS_LOCAL="${OVMF_DIR}/ovmf-vars-${KARCH}.fd"

        mkdir -p "${OVMF_DIR}"

        if [ ! -f "${OVMF_VARS_LOCAL}" ]; then
            cp /usr/share/edk2/x64/OVMF_VARS.fd "${OVMF_VARS_LOCAL}"
        fi

        OVMF_VARS="${OVMF_VARS_LOCAL}"
        return
    fi

    if [ -f "${OVMF_DIR}/ovmf-code-${KARCH}.fd" ]; then
        OVMF_CODE="${OVMF_DIR}/ovmf-code-${KARCH}.fd"
        OVMF_VARS="${OVMF_DIR}/ovmf-vars-${KARCH}.fd"
        return
    fi

    echo "OVMF not found"
    exit 1
}

run_qemu() {
    [ -f "${IMAGE_NAME}.iso" ] || exit 1

    find_ovmf

    echo "Launching QEMU"

    qemu-system-${KARCH} \
        -M q35 \
        -m 2G \
        -drive if=pflash,unit=0,format=raw,file=${OVMF_CODE},readonly=on \
        -drive if=pflash,unit=1,format=raw,file=${OVMF_VARS} \
        -cdrom ${IMAGE_NAME}.iso \
        -drive file=${DISK_DIR}/ide_disk.img,format=raw,if=ide,index=0,media=disk \
        -drive file=${DISK_DIR}/ahci_disk.img,format=raw,if=none,id=ahci0 \
        -device ahci,id=ahci \
        -device ide-hd,drive=ahci0,bus=ahci.0 \
        -smp 4 -m 500M -d int,cpu_reset
}

run_qemu_codespace() {
    [ -f "${IMAGE_NAME}.iso" ] || exit 1

    find_ovmf

    qemu-system-${KARCH} \
        -M q35 \
        -m 2G \
        -drive if=pflash,unit=0,format=raw,file=${OVMF_CODE},readonly=on \
        -drive if=pflash,unit=1,format=raw,file=${OVMF_VARS} \
        -drive file=${IMAGE_NAME}.iso,format=raw,if=ide,index=0,media=cdrom \
        -drive file=${DISK_DIR}/ide_disk.img,format=raw,if=ide,index=1,media=disk \
        -drive file=${DISK_DIR}/ahci_disk.img,format=raw,if=none,id=ahci0 \
        -device ahci,id=ahci \
        -device ide-hd,drive=ahci0,bus=ahci.0 \
        -smp 4
}

clean() {
    make -C kernel clean
    rm -rf "${ISO_ROOT}"
    rm -f "${IMAGE_NAME}.iso"
}

distclean() {
    clean
    rm -rf "${LIMINE_DIR}"
    rm -rf "${DISK_DIR}"
}

case "${1:-}" in
    build)
        build_kernel
        create_disks
        build_iso
        ;;
    run)
        build_kernel
        create_disks
        build_iso
        run_qemu
        ;;
    run-codespace)
        build_kernel
        create_disks
        build_iso
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
        exit 1
        ;;
esac