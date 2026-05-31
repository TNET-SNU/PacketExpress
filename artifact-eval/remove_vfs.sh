#!/usr/bin/env bash
# remove_vfs.sh — tear down all SR-IOV VFs before running the AE scripts.
#
# bessd / nat / dpvs are launched without an explicit DPDK PCI allowlist
# (-a 0000:..), so DPDK auto-probes every device it can bind. Leftover VFs
# (eth0vf0, eth1vf0, ...) get grabbed and cause launch bugs. Writing 0 to a
# PF's sriov_numvfs destroys its VF PCI devices entirely, so DPDK never sees
# them. After this runs, only the PFs (eth0..eth3) remain.
#
# Idempotent and best-effort: PFs without VFs are skipped, errors don't abort.
# Run as root (or via sudo); the AE run scripts can `source`/call this first.
set -uo pipefail

# PFs to clear. Override: PFS="eth0 eth1" ./remove_vfs.sh
PFS="${PFS:-eth0 eth1 eth2 eth3}"

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo -n"
fi

log() { echo "[remove_vfs] $*" >&2; }

removed_any=0
for pf in $PFS; do
    nv="/sys/class/net/${pf}/device/sriov_numvfs"
    if [ ! -e "$nv" ]; then
        log "${pf}: no sriov_numvfs (no PF / SR-IOV unsupported) — skip"
        continue
    fi

    cur="$(cat "$nv" 2>/dev/null || echo 0)"
    if [ "$cur" -eq 0 ] 2>/dev/null; then
        log "${pf}: already 0 VFs — skip"
        continue
    fi

    log "${pf}: removing ${cur} VF(s)"

    # Unbind each VF from its driver first. Zeroing numvfs while a VF is still
    # bound (esp. to vfio-pci held by a dead-but-not-reaped process) can EBUSY;
    # unbinding makes the teardown reliable.
    for vf in /sys/class/net/"${pf}"/device/virtfn*; do
        [ -e "$vf" ] || continue
        pci="$(basename "$(readlink -f "$vf")")"
        if [ -e "$vf/driver/unbind" ]; then
            log "  unbind VF ${pci} from $(basename "$(readlink -f "$vf/driver")")"
            echo "$pci" | $SUDO tee "$vf/driver/unbind" >/dev/null 2>&1 || true
        fi
    done

    # Destroy the VFs.
    echo 0 | $SUDO tee "$nv" >/dev/null 2>&1 || true

    new="$(cat "$nv" 2>/dev/null || echo '?')"
    if [ "$new" = "0" ]; then
        log "${pf}: OK, now 0 VFs"
        removed_any=1
    else
        log "${pf}: WARNING numvfs is still '${new}' (a VF may be in use — kill bessd/nat/dpvs first)"
    fi
done

# Safety net: report any VF still present anywhere so it can't silently get
# probed by DPDK. (sriov_vf == 1 marks a device as a VF.)
leftover=0
for d in /sys/bus/pci/devices/*; do
    [ -e "$d/sriov_vf" ] || continue
    # Newer kernels expose physfn symlink on VFs; use that as the VF marker.
    if [ -e "$d/physfn" ]; then
        log "LEFTOVER VF still present: $(basename "$d") -> bound to $(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)"
        leftover=1
    fi
done

if [ "$leftover" -eq 0 ]; then
    log "done — no VFs remain, only PFs (${PFS}) are present"
else
    log "done — but some VFs remain (see LEFTOVER above)"
fi
exit 0
