#!/bin/bash
# SPDX-License-Identifier: GPL-3.0+
# Standalone NVMe-FC ANA Multipath Test Script
# Based on blktests nvme/057
#
# Usage:
#   ./nvmet_fc_ana_test.sh setup    - Configure and start nvmet_fc subsystem
#   ./nvmet_fc_ana_test.sh test     - Run ANA failover test with I/O
#   ./nvmet_fc_ana_test.sh cleanup  - Stop and destroy nvmet_fc subsystem
#   ./nvmet_fc_ana_test.sh all      - Run complete test (setup -> test -> cleanup)

set -e

# Configuration variables
NVMET_CFS="/sys/kernel/config/nvmet/"
NVME_IMG_SIZE="1G"
TEST_IMG="${TMPDIR:-/tmp}/nvmet_fc_test_img"
DEF_SUBSYSNQN="blktests-subsystem-1"
DEF_SUBSYS_UUID="91fdba0d-f87b-4c25-b80f-db7be1418b9e"
DEF_HOSTID="0f01fb42-9f7f-4856-b0b3-51e60b8de349"
DEF_HOSTNQN="nqn.2014-08.org.nvmexpress:uuid:${DEF_HOSTID}"
DEF_LOCAL_WWNN="0x10001100aa000001"
DEF_LOCAL_WWPN="0x20001100aa000001"
DEF_REMOTE_WWNN="0x10001100ab000001"
DEF_REMOTE_WWPN="0x20001100ab000001"

# Array to store created ports
declare -a NVMET_PORTS

print_usage() {
    echo "Usage: $0 {setup|test|cleanup|all}"
    echo ""
    echo "Commands:"
    echo "  setup   - Configure and start nvmet_fc subsystem with ANA"
    echo "  test    - Run ANA failover test with background I/O"
    echo "  cleanup - Stop and destroy nvmet_fc subsystem"
    echo "  all     - Run complete test sequence"
    echo ""
    echo "Environment variables:"
    echo "  NVME_IMG_SIZE - Size of backing storage (default: 1G)"
}

check_requirements() {
    # Check for root privileges
    if [[ $EUID -ne 0 ]]; then
        echo "ERROR: This script requires root privileges"
        exit 1
    fi

    # Check for required modules
    local required_modules="nvmet nvme-fc nvme-fcloop"
    for mod in $required_modules; do
        if ! lsmod | grep -q "^${mod}"; then
            echo "Loading module: $mod"
            modprobe "$mod" 2>/dev/null || {
                echo "ERROR: Failed to load module $mod"
                exit 1
            }
        fi
    done

    # Check for required tools
    local required_tools="fio nvme losetup"
    for tool in $required_tools; do
        if ! command -v "$tool" &> /dev/null; then
            echo "ERROR: Required tool '$tool' not found"
            exit 1
        fi
    done

    # Check for configfs
    if [[ ! -d "$NVMET_CFS" ]]; then
        echo "ERROR: NVMe target configfs not available at $NVMET_CFS"
        exit 1
    fi
}

# Helper functions from blktests
remote_wwnn() {
    local -i port=${1}
    printf "0x%08x" $(( DEF_REMOTE_WWNN + port ))
}

remote_wwpn() {
    local -i port=${1}
    printf "0x%08x" $(( DEF_REMOTE_WWPN + port ))
}

fc_traddr() {
    printf "nn-%s:pn-%s" "$(remote_wwnn "$1")" "$(remote_wwpn "$1")"
}

nvme_fcloop_add_lport() {
    local wwnn="$1"
    local wwpn="$2"
    local loopctl=/sys/class/fcloop/ctl

    echo "wwnn=${wwnn},wwpn=${wwpn}" > ${loopctl}/add_local_port
}

nvme_fcloop_add_tport() {
    local wwnn="$1"
    local wwpn="$2"
    local loopctl=/sys/class/fcloop/ctl

    echo "wwnn=${wwnn},wwpn=${wwpn}" > ${loopctl}/add_target_port
}

nvme_fcloop_add_rport() {
    local local_wwnn="$1"
    local local_wwpn="$2"
    local remote_wwnn="$3"
    local remote_wwpn="$4"
    local loopctl=/sys/class/fcloop/ctl

    echo "wwnn=${remote_wwnn},wwpn=${remote_wwpn},lpwwnn=${local_wwnn},lpwwpn=${local_wwpn},roles=0x60" > ${loopctl}/add_remote_port
}

nvme_fcloop_del_lport() {
    local wwnn="$1"
    local wwpn="$2"
    local loopctl=/sys/class/fcloop/ctl

    if [[ -f "${loopctl}/del_local_port" ]]; then
        echo "wwnn=${wwnn},wwpn=${wwpn}" > "${loopctl}/del_local_port"
    fi
}

nvme_fcloop_del_tport() {
    local wwnn="$1"
    local wwpn="$2"
    local loopctl=/sys/class/fcloop/ctl

    if [[ -f "${loopctl}/del_target_port" ]]; then
        echo "wwnn=${wwnn},wwpn=${wwpn}" > "${loopctl}/del_target_port"
    fi
}

nvme_fcloop_del_rport() {
    local local_wwnn="$1"
    local local_wwpn="$2"
    local remote_wwnn="$3"
    local remote_wwpn="$4"
    local loopctl=/sys/class/fcloop/ctl

    if [[ -f "${loopctl}/del_remote_port" ]]; then
        echo "wwnn=${remote_wwnn},wwpn=${remote_wwpn}" > "${loopctl}/del_remote_port"
    fi
}

create_nvmet_port() {
    local port
    for ((port = 0; ; port++)); do
        if [[ ! -e "${NVMET_CFS}/ports/${port}" ]]; then
            break
        fi
    done

    local portcfs="${NVMET_CFS}/ports/${port}"
    mkdir "${portcfs}"

    # Configure FC port
    nvme_fcloop_add_tport "$(remote_wwnn $port)" "$(remote_wwpn $port)"
    nvme_fcloop_add_rport "$DEF_LOCAL_WWNN" "$DEF_LOCAL_WWPN" \
                          "$(remote_wwnn $port)" "$(remote_wwpn $port)"

    echo "fc" > "${portcfs}/addr_trtype"
    echo "$(fc_traddr $port)" > "${portcfs}/addr_traddr"
    echo "fc" > "${portcfs}/addr_adrfam"

    echo "$port"
}

setup_nvmet_port_ana() {
    local port="$1"
    local anagrpid="${2:-1}"
    local anastate="${3:-optimized}"
    local cfsport="${NVMET_CFS}/ports/${port}"
    local anaport="${cfsport}/ana_groups/${anagrpid}"

    if [[ ! -d "${anaport}" ]] ; then
        if [[ "${anagrpid}" -eq 1 ]]; then
            echo "ERROR: ANA not supported"
            exit 1
        fi
        mkdir "${anaport}"
    fi
    echo "${anastate}" > "${anaport}/ana_state"
}

create_nvmet_subsystem() {
    local subsystem="$DEF_SUBSYSNQN"
    local blkdev="$1"
    local uuid="$DEF_SUBSYS_UUID"
    local cfs_path="${NVMET_CFS}/subsystems/${subsystem}"

    mkdir -p "${cfs_path}"
    echo 0 > "${cfs_path}/attr_allow_any_host"

    # Create namespace
    local ns_path="${cfs_path}/namespaces/1"
    mkdir "${ns_path}"
    printf "%s" "${blkdev}" > "${ns_path}/device_path"
    printf "%s" "${uuid}" > "${ns_path}/device_uuid"
    printf 1 > "${ns_path}/enable"
}

add_nvmet_subsys_to_port() {
    local port="$1"
    local nvmet_subsystem="$DEF_SUBSYSNQN"

    ln -s "${NVMET_CFS}/subsystems/${nvmet_subsystem}" \
        "${NVMET_CFS}/ports/${port}/subsystems/${nvmet_subsystem}"
}

create_nvmet_host() {
    local nvmet_subsystem="$DEF_SUBSYSNQN"
    local nvmet_hostnqn="$DEF_HOSTNQN"
    local host_path="${NVMET_CFS}/hosts/${nvmet_hostnqn}"
    local cfs_path="${NVMET_CFS}/subsystems/${nvmet_subsystem}"

    mkdir "${host_path}"
    ln -s "${host_path}" "${cfs_path}/allowed_hosts/${nvmet_hostnqn}"
}

find_nvme_dev() {
    local subsys="$DEF_SUBSYSNQN"
    local subsysnqn
    local dev

    for dev in /sys/class/nvme/nvme*; do
        [ -e "$dev" ] || continue
        dev="$(basename "$dev")"
        subsysnqn="$(cat "/sys/class/nvme/${dev}/subsysnqn" 2>/dev/null)"
        if [[ "$subsysnqn" == "$subsys" ]]; then
            echo "$dev"
            return 0
        fi
    done
    return 1
}

find_nvme_ns() {
    local subsys_uuid="$DEF_SUBSYS_UUID"
    local uuid
    local ns

    for ns in "/sys/block/nvme"* ; do
        if ! [[ "${ns}" =~ nvme[0-9]+n[0-9]+ ]]; then
            continue
        fi
        [ -e "${ns}/uuid" ] || continue
        uuid=$(cat "${ns}/uuid")
        if [[ "${subsys_uuid}" == "${uuid}" ]]; then
            basename "${ns}"
            return 0
        fi
    done
    return 1
}

setup_nvmet_target() {
    echo "=== Setting up NVMe-FC target ==="

    check_requirements

    # Create backing storage
    echo "Creating backing storage: $TEST_IMG ($NVME_IMG_SIZE)"
    truncate -s "$NVME_IMG_SIZE" "$TEST_IMG"
    local blkdev="$(losetup -f --show "$TEST_IMG")"
    echo "Using loop device: $blkdev"

    # Setup FC loop infrastructure
    echo "Setting up FC loop infrastructure"
    nvme_fcloop_add_lport "$DEF_LOCAL_WWNN" "$DEF_LOCAL_WWPN"

    # Create subsystem
    echo "Creating NVMe target subsystem: $DEF_SUBSYSNQN"
    create_nvmet_subsystem "$blkdev"

    # Create 4 ports for ANA testing
    echo "Creating 4 ports for ANA multipath"
    for ((i = 0; i < 4; i++)); do
        port=$(create_nvmet_port)
        NVMET_PORTS+=("$port")
        add_nvmet_subsys_to_port "$port"
        echo "Created port $port"
    done

    # Create host
    echo "Creating host configuration"
    create_nvmet_host

    # Set initial ANA states (failback configuration)
    echo "Setting initial ANA states"
    setup_nvmet_port_ana "${NVMET_PORTS[0]}" 1 "optimized"
    setup_nvmet_port_ana "${NVMET_PORTS[1]}" 1 "non-optimized"
    setup_nvmet_port_ana "${NVMET_PORTS[2]}" 1 "inaccessible"
    setup_nvmet_port_ana "${NVMET_PORTS[3]}" 1 "inaccessible"

    echo "NVMe-FC target setup complete!"
    echo "Ports created: ${NVMET_PORTS[*]}"
    echo "Backing device: $blkdev"
}

connect_initiator() {
    echo "=== Connecting NVMe-FC initiator ==="

    # Connect to each port
    for port in "${NVMET_PORTS[@]}"; do
        echo "Connecting to port $port"
        nvme connect \
            --transport fc \
            --traddr "$(fc_traddr "$port")" \
            --host-traddr "nn-${DEF_LOCAL_WWNN}:pn-${DEF_LOCAL_WWPN}" \
            --nqn "$DEF_SUBSYSNQN" \
            --hostnqn "$DEF_HOSTNQN" \
            --hostid "$DEF_HOSTID"
    done

    # Wait for device to be ready
    local nvmedev ns
    for ((i = 0; i < 10; i++)); do
        nvmedev=$(find_nvme_dev)
        if [[ -n "$nvmedev" ]]; then
            ns=$(find_nvme_ns)
            if [[ -n "$ns" ]]; then
                echo "Connected to NVMe device: $nvmedev, namespace: $ns"
                return 0
            fi
        fi
        sleep 1
    done
    echo "ERROR: Failed to connect to NVMe device"
    return 1
}

ana_failover_test() {
    echo "=== Running ANA Failover Test ==="

    connect_initiator

    local nvmedev ns
    nvmedev=$(find_nvme_dev)
    ns=$(find_nvme_ns)

    if [[ -z "$nvmedev" || -z "$ns" ]]; then
        echo "ERROR: NVMe device not found"
        return 1
    fi

    echo "Starting background I/O on /dev/$ns"
    fio --name=ana_test \
        --filename="/dev/$ns" \
        --rw=randwrite \
        --direct=1 \
        --ioengine=libaio \
        --bs=4k \
        --iodepth=16 \
        --verify=crc32c \
        --verify_state_save=0 \
        --group_reporting \
        --ramp_time=5 \
        --time_based \
        --runtime=60 \
        --output-format=json \
        --output=ana_test_results.json &> /dev/null &

    local fio_pid=$!
    sleep 10

    echo "Performing ANA failover (switching optimized paths)"
    # Failover: make ports 2,3 optimized, 0,1 inaccessible
    setup_nvmet_port_ana "${NVMET_PORTS[0]}" 1 "inaccessible"
    setup_nvmet_port_ana "${NVMET_PORTS[1]}" 1 "inaccessible"
    setup_nvmet_port_ana "${NVMET_PORTS[2]}" 1 "optimized"
    setup_nvmet_port_ana "${NVMET_PORTS[3]}" 1 "non-optimized"

    sleep 15

    echo "Performing ANA failback (switching back to original state)"
    # Failback: restore original state
    setup_nvmet_port_ana "${NVMET_PORTS[0]}" 1 "optimized"
    setup_nvmet_port_ana "${NVMET_PORTS[1]}" 1 "non-optimized"
    setup_nvmet_port_ana "${NVMET_PORTS[2]}" 1 "inaccessible"
    setup_nvmet_port_ana "${NVMET_PORTS[3]}" 1 "inaccessible"

    sleep 15

    echo "Stopping background I/O"
    { kill "$fio_pid"; wait; } &> /dev/null

    echo "Disconnecting NVMe initiator"
    nvme disconnect --nqn "$DEF_SUBSYSNQN"

    echo "ANA failover test completed successfully!"
}

cleanup_nvmet_target() {
    echo "=== Cleaning up NVMe-FC target ==="

    # Disconnect any remaining connections
    nvme disconnect --nqn "$DEF_SUBSYSNQN" 2>/dev/null || true

    # Remove ports
    for port in "${NVMET_PORTS[@]}"; do
        if [[ -n "$port" && -d "${NVMET_CFS}/ports/${port}" ]]; then
            echo "Removing port $port"

            # Remove subsystem from port
            rm -f "${NVMET_CFS}/ports/${port}/subsystems/${DEF_SUBSYSNQN}" 2>/dev/null || true

            # Remove FC loop components
            nvme_fcloop_del_rport "$DEF_LOCAL_WWNN" "$DEF_LOCAL_WWPN" \
                                  "$(remote_wwnn "$port")" "$(remote_wwpn "$port")"
            nvme_fcloop_del_tport "$(remote_wwnn "$port")" "$(remote_wwpn "$port")"

            # Remove ANA groups
            rm -rf "${NVMET_CFS}/ports/${port}/ana_groups/"* 2>/dev/null || true

            # Remove port directory
            rmdir "${NVMET_CFS}/ports/${port}" 2>/dev/null || true
        fi
    done

    # Remove subsystem
    if [[ -d "${NVMET_CFS}/subsystems/${DEF_SUBSYSNQN}" ]]; then
        echo "Removing subsystem $DEF_SUBSYSNQN"
        echo 0 > "${NVMET_CFS}/subsystems/${DEF_SUBSYSNQN}/namespaces/1/enable" 2>/dev/null || true
        rmdir "${NVMET_CFS}/subsystems/${DEF_SUBSYSNQN}/namespaces/1" 2>/dev/null || true
        rm -f "${NVMET_CFS}/subsystems/${DEF_SUBSYSNQN}/allowed_hosts/"* 2>/dev/null || true
        rmdir "${NVMET_CFS}/subsystems/${DEF_SUBSYSNQN}" 2>/dev/null || true
    fi

    # Remove host
    if [[ -d "${NVMET_CFS}/hosts/${DEF_HOSTNQN}" ]]; then
        echo "Removing host $DEF_HOSTNQN"
        rmdir "${NVMET_CFS}/hosts/${DEF_HOSTNQN}" 2>/dev/null || true
    fi

    # Remove FC local port
    nvme_fcloop_del_lport "$DEF_LOCAL_WWNN" "$DEF_LOCAL_WWPN"

    # Clean up loop device and backing file
    local loopdevs
    loopdevs=$(losetup -l | awk -v img="$TEST_IMG" '$6 == img { print $1 }')
    for dev in $loopdevs; do
        echo "Removing loop device: $dev"
        losetup -d "$dev"
    done

    if [[ -f "$TEST_IMG" ]]; then
        echo "Removing backing file: $TEST_IMG"
        rm -f "$TEST_IMG"
    fi

    echo "Cleanup completed!"
}

# Main script logic
case "$1" in
    setup)
        setup_nvmet_target
        ;;
    test)
        ana_failover_test
        ;;
    cleanup)
        cleanup_nvmet_target
        ;;
    all)
        echo "Running complete ANA test sequence..."
        setup_nvmet_target
        ana_failover_test
        cleanup_nvmet_target
        echo "Complete test sequence finished!"
        ;;
    *)
        print_usage
        exit 1
        ;;
esac
