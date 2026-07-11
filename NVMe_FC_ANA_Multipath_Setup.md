# NVMe-FC ANA Multipath Setup Analysis

## Overview

This document explains how the blktests `nvme/057` script creates a running ANA (Asymmetric Namespace Access) multipath NVMe-FC subsystem. The script demonstrates NVMe Fabrics controller failover during I/O operations using FC transport with synthetic fcloop devices.

## What is ANA (Asymmetric Namespace Access)?

ANA is an NVMe feature that allows a subsystem to report different access characteristics for namespaces through different controllers/paths. It enables intelligent multipath I/O routing based on path states:

- **Optimized**: Best performance, preferred path
- **Non-optimized**: Available but with potentially reduced performance
- **Inaccessible**: Path cannot access the namespace
- **Persistent Loss**: Path permanently cannot access the namespace

## High-Level Test Flow

```
1. Setup NVMe Target Infrastructure
   ↓
2. Create 4 FC Ports with ANA Groups
   ↓
3. Set Initial ANA States (failback config)
   ↓
4. Connect NVMe Initiator to All Ports
   ↓
5. Start Background I/O
   ↓
6. Change ANA States (failover)
   ↓
7. Change ANA States Back (failback)
   ↓
8. Stop I/O and Cleanup
```

## Detailed Step-by-Step Analysis

### Step 1: Infrastructure Setup

```bash
_setup_nvmet()
```

**Purpose**: Initialize NVMe target infrastructure
**Actions**:
- Loads required kernel modules (`nvmet`, `nvme-fc`, `nvme-fcloop`)
- Sets up FC loop infrastructure (synthetic FC transport for testing)
- Creates FC local port with predefined WWNN/WWPN

**Key Components**:
- **fcloop**: Synthetic FC transport driver for testing
- **Local Port**: `WWNN=0x10001100aa000001, WWPN=0x20001100aa000001`
- **ConfigFS Mount**: `/sys/kernel/config/nvmet/` for target configuration

### Step 2: Target Subsystem Creation

```bash
_nvmet_target_setup --ports 4
```

**Purpose**: Create NVMe target subsystem with 4 FC ports
**Actions**:
1. **Create Backing Storage**:
   - Creates 1GB file: `/tmp/img`
   - Sets up loop device: `/dev/loopX`

2. **Create NVMe Subsystem**:
   - Subsystem NQN: `blktests-subsystem-1`
   - Subsystem UUID: `91fdba0d-f87b-4c25-b80f-db7be1418b9e`
   - Creates namespace with backing storage

3. **Create 4 FC Ports**:
   ```
   Port 0: WWNN=0x10001100ab000000, WWPN=0x20001100ab000000
   Port 1: WWNN=0x10001100ab000001, WWPN=0x20001100ab000001
   Port 2: WWNN=0x10001100ab000002, WWPN=0x20001100ab000002
   Port 3: WWNN=0x10001100ab000003, WWPN=0x20001100ab000003
   ```

4. **FC Loop Setup Per Port**:
   - Creates target port (WWNN/WWPN)
   - Creates remote port connection
   - Sets transport type to "fc"
   - Configures FC address (`nn-<WWNN>:pn-<WWPN>`)

5. **Link Subsystem to Ports**:
   ```bash
   ln -s /sys/kernel/config/nvmet/subsystems/blktests-subsystem-1 \
         /sys/kernel/config/nvmet/ports/$port/subsystems/blktests-subsystem-1
   ```

6. **Create Host Configuration**:
   - Host NQN: `nqn.2014-08.org.nvmexpress:uuid:<hostid>`
   - Links host to subsystem allowed_hosts

### Step 3: ANA Configuration - Initial State (Failback)

```bash
failback "${ports[@]}"
```

**Purpose**: Set initial ANA states for multipath testing
**Configuration**:
- **Port 0**: ANA Group 1, State = "optimized" (primary path)
- **Port 1**: ANA Group 1, State = "non-optimized" (secondary path)
- **Port 2**: ANA Group 1, State = "inaccessible" (unavailable)
- **Port 3**: ANA Group 1, State = "inaccessible" (unavailable)

**Implementation**:
```bash
_setup_nvmet_port_ana() {
    local port="$1"
    local anagrpid="${2:-1}"  # ANA Group ID
    local anastate="${3:-optimized}"  # ANA State
    local cfsport="${NVMET_CFS}/ports/${port}"
    local anaport="${cfsport}/ana_groups/${anagrpid}"

    mkdir -p "${anaport}"
    echo "${anastate}" > "${anaport}/ana_state"
}
```

### Step 4: Initiator Connection

```bash
for port in "${ports[@]}"; do
    _nvme_connect_subsys --port "${port}" --no-wait-ns
done
```

**Purpose**: Connect NVMe initiator to all 4 target ports
**Result**: Creates multipath device with 4 potential paths

**Connection Details Per Port**:
```bash
nvme connect \
    --transport fc \
    --traddr "nn-<target_wwnn>:pn-<target_wwpn>" \
    --host-traddr "nn-<local_wwnn>:pn-<local_wwpn>" \
    --nqn "blktests-subsystem-1" \
    --hostnqn "nqn.2014-08.org.nvmexpress:uuid:<hostid>" \
    --hostid "<hostid>"
```

**Multipath Behavior**:
- NVMe core creates single namespace device (e.g., `nvme0n1`)
- Multiple controllers created (`nvme0`, `nvme1`, `nvme2`, `nvme3`)
- ANA states determine active I/O paths:
  - Port 0: Primary (optimized)
  - Port 1: Secondary (non-optimized)
  - Ports 2,3: Unavailable (inaccessible)

### Step 5: I/O Testing

```bash
ns=$(_find_nvme_ns "$def_subsys_uuid")
_run_fio_verify_io --filename="/dev/${ns}" \
    --group_reporting --ramp_time=5 \
    --time_based --runtime=1m &> "$FULL" &
fio_pid=$!
```

**Purpose**: Generate continuous I/O during ANA state changes
**I/O Pattern**:
- **Tool**: fio (Flexible I/O Tester)
- **Pattern**: Random write with verification
- **Engine**: libaio (Linux Async I/O)
- **Block Size**: 4KB
- **Queue Depth**: 16
- **Verification**: CRC32C checksum
- **Duration**: 1 minute
- **Behavior**: I/O routes through optimized path (Port 0)

### Step 6: ANA Failover

```bash
echo "ANA failover"
failover "${ports[@]}"
```

**Purpose**: Simulate path failure by changing ANA states
**New Configuration**:
- **Port 0**: ANA Group 1, State = "inaccessible" (failed)
- **Port 1**: ANA Group 1, State = "inaccessible" (failed)
- **Port 2**: ANA Group 1, State = "optimized" (new primary)
- **Port 3**: ANA Group 1, State = "non-optimized" (new secondary)

**Expected Behavior**:
1. NVMe multipath detects ANA state change
2. I/O automatically fails over to Port 2 (new optimized path)
3. Minimal I/O disruption during transition
4. Background fio continues running

### Step 7: ANA Failback

```bash
echo "ANA failback"
failback "${ports[@]}"
```

**Purpose**: Restore original path configuration
**Restored Configuration**:
- **Port 0**: ANA Group 1, State = "optimized" (restored primary)
- **Port 1**: ANA Group 1, State = "non-optimized" (restored secondary)
- **Port 2**: ANA Group 1, State = "inaccessible" (back to unavailable)
- **Port 3**: ANA Group 1, State = "inaccessible" (back to unavailable)

**Expected Behavior**:
1. I/O fails back to Port 0 (original optimized path)
2. Port 2 becomes unavailable for new I/O
3. Seamless transition with ongoing I/O

### Step 8: Cleanup

```bash
{ kill "${fio_pid}"; wait; } &> /dev/null
_nvme_disconnect_subsys
_nvmet_target_cleanup
```

**Purpose**: Clean shutdown of test environment
**Actions**:
1. **Stop I/O**: Terminate fio background process
2. **Disconnect Initiator**: `nvme disconnect --nqn "blktests-subsystem-1"`
3. **Cleanup Target**:
   - Remove subsystem from ports
   - Delete FC loop remote/target ports
   - Remove ANA groups
   - Delete port directories
   - Remove subsystem and namespaces
   - Remove host configuration
   - Delete FC local port
   - Cleanup loop device and backing file

## Key Configuration Files and Paths

### NVMe Target ConfigFS Structure
```
/sys/kernel/config/nvmet/
├── hosts/
│   └── nqn.2014-08.org.nvmexpress:uuid:<hostid>/
├── ports/
│   ├── 0/
│   │   ├── addr_trtype → "fc"
│   │   ├── addr_traddr → "nn-<wwnn>:pn-<wwpn>"
│   │   ├── addr_adrfam → "fc"
│   │   ├── ana_groups/
│   │   │   └── 1/
│   │   │       └── ana_state → "optimized|non-optimized|inaccessible"
│   │   └── subsystems/
│   │       └── blktests-subsystem-1 → symlink
│   ├── 1/ ...
│   ├── 2/ ...
│   └── 3/ ...
└── subsystems/
    └── blktests-subsystem-1/
        ├── attr_allow_any_host → "0"
        ├── allowed_hosts/
        │   └── <hostnqn> → symlink
        └── namespaces/
            └── 1/
                ├── device_path → "/dev/loop0"
                ├── device_uuid → "91fdba0d-f87b-4c25-b80f-db7be1418b9e"
                └── enable → "1"
```

### FC Loop Control Interface
```
/sys/class/fcloop/ctl/
├── add_local_port
├── add_target_port
├── add_remote_port
├── del_local_port
├── del_target_port
└── del_remote_port
```

## ANA State Machine and Multipath Behavior

### Path Selection Logic
1. **Optimized paths**: Preferred for new I/O
2. **Non-optimized paths**: Used when optimized unavailable
3. **Inaccessible paths**: Not used for I/O
4. **Load balancing**: Among paths of same state

### Failover Sequence
```
Initial: Port0(opt) + Port1(non-opt) → I/O on Port0
   ↓
Failover: Port2(opt) + Port3(non-opt) → I/O switches to Port2
   ↓
Failback: Port0(opt) + Port1(non-opt) → I/O returns to Port0
```

## Test Validation

The test validates:
- ✅ **ANA State Configuration**: Proper setup of optimized/non-optimized/inaccessible states
- ✅ **Multipath Path Discovery**: All 4 paths detected by initiator
- ✅ **Intelligent Path Selection**: I/O routes to optimized paths
- ✅ **Seamless Failover**: I/O continues during ANA state changes
- ✅ **Path Recovery**: Proper failback when optimal paths restored
- ✅ **I/O Integrity**: Data verification throughout test

## Summary

The `nvme/057` test demonstrates a comprehensive NVMe-FC ANA multipath setup that:

1. **Creates realistic multipath environment** with 4 FC paths
2. **Uses synthetic FC transport** (fcloop) for reproducible testing
3. **Implements proper ANA state management** for path optimization
4. **Tests failover/failback scenarios** with continuous I/O
5. **Validates multipath intelligence** and I/O path selection
6. **Ensures data integrity** throughout state transitions

This provides thorough testing of NVMe-FC multipath functionality in a controlled, synthetic environment without requiring physical FC infrastructure.
