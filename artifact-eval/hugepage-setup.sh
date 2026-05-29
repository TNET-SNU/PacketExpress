#!/bin/bash
set -euo pipefail

# Re-exec this script as root if not already root.
if [[ $EUID -ne 0 ]]; then
    exec sudo -n "$0" "$@"
fi

NR_1G_PER_NODE="${NR_1G_PER_NODE:-64}"   # 64 GB per NUMA node => 128 GB total

# 1. Best-effort: kill any leftover DPDK app holding hugepages.
pkill -KILL -x dpvs    >/dev/null 2>&1 || true
pkill -KILL -x bessd   >/dev/null 2>&1 || true
for n in 0 1 2 3 4; do
  pkill -KILL -x "bessd${n}" >/dev/null 2>&1 || true
  pkill -KILL -x "nat${n}"   >/dev/null 2>&1 || true
done
pkill -KILL -f 'pxgw-hdr-only' >/dev/null 2>&1 || true
sleep 1

# 2. Wipe stale rtemap_* files in every hugetlbfs mount. These are leftover
#    page handles from DPDK apps that crashed; while they exist the kernel
#    cannot release the underlying huge pages, so `echo 0 > nr_hugepages`
#    looks like it does nothing.
while read -r src mnt type opts _; do
  if [[ "$type" == "hugetlbfs" ]]; then
    rm -f "${mnt}"/rtemap_* "${mnt}"/.rte_config "${mnt}"/.rte_hugepage_info \
          >/dev/null 2>&1 || true
  fi
done < /proc/mounts

# 3. Drop all hugepages first, then reallocate. Doing it in two passes makes
#    sure any in-use reservations are released before the new count is set.
for n in 0 1; do
  for sz in 2048kB 1048576kB; do
    f="/sys/devices/system/node/node${n}/hugepages/hugepages-${sz}/nr_hugepages"
    [[ -f "$f" ]] && echo 0 > "$f"
  done
done

# 4. Reallocate: 0 x 2 MB, ${NR_1G_PER_NODE} x 1 GB per NUMA node.
for n in 0 1; do
  echo "$NR_1G_PER_NODE" > "/sys/devices/system/node/node${n}/hugepages/hugepages-1048576kB/nr_hugepages"
done

# 5. Report.
echo "[INFO] hugepage state after setup:"
for n in 0 1; do
  for sz in 2048kB 1048576kB; do
    f="/sys/devices/system/node/node${n}/hugepages/hugepages-${sz}/nr_hugepages"
    fr="/sys/devices/system/node/node${n}/hugepages/hugepages-${sz}/free_hugepages"
    if [[ -f "$f" ]]; then
      printf '  node%d %s: nr=%s free=%s\n' \
        "$n" "$sz" "$(cat "$f")" "$(cat "$fr" 2>/dev/null || echo '?')"
    fi
  done
done
grep -E 'HugePages_(Total|Free|Rsvd)|Hugepagesize' /proc/meminfo |
  sed 's/^/  /'
