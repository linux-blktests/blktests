# NVMe FC Marginal Path Test (nvme/067)

## Overview

I have created a new blktests test `nvme/067` that tests the `NVME_CTRL_MARGINAL` flag functionality by simulating FC link degradation through sysfs manipulation. This test is based on the analysis of the NVME_CTRL_MARGINAL flag behavior documented in `NVME_CTRL_MARGINAL_Analysis.md`.

## Test Description

**Test Name**: `nvme/067`
**Description**: "test nvme-fc marginal path handling"
**Transport**: FC (uses fcloop device)

## What The Test Does

1. **Setup Phase**:
   - Sets up NVMe-FC target using fcloop (fake FC transport)
   - Creates a single port configuration
   - Connects NVMe initiator to the target
   - Identifies the FC remote port and NVMe controller

2. **Initial State Verification**:
   - Verifies FC remote port is in "Online" state
   - Confirms NVMe controller is not marked as marginal

3. **Marginal Path Simulation**:
   - Starts background I/O using fio
   - Sets FC remote port state to "Marginal" via sysfs
   - Monitors NVMe controller for marginal state detection
   - Allows I/O to continue under marginal conditions

4. **Recovery Testing**:
   - Sets FC remote port back to "Online" state
   - Waits for NVMe controller to return to "live" state
   - Verifies I/O completion

5. **Cleanup**:
   - Stops background I/O
   - Disconnects from target
   - Cleans up target configuration

## Key Functions

### FC Remote Port Management
- `_find_fc_rport()` - Locates FC remote port by WWPN
- `_set_fc_rport_state()` - Sets port state (Online/Marginal/Blocked)
- `_get_fc_rport_state()` - Reads current port state

### NVMe Controller State Monitoring
- `_nvme_ctrl_is_marginal()` - Checks if controller is marked marginal
- `_wait_for_ctrl_state()` - Waits for specific controller state

## Test Flow

```
Setup NVMe-FC Target (fcloop)
    ↓
Connect NVMe Initiator
    ↓
Start Background I/O
    ↓
Find FC Remote Port via sysfs (/sys/class/fc_remote_ports/rport-*)
    ↓
Set port_state = "Marginal"
    ↓
Monitor /sys/class/nvme/nvmeX/state for "marginal"
    ↓
Verify NVME_CTRL_MARGINAL flag impacts I/O behavior
    ↓
Set port_state = "Online"
    ↓
Monitor controller return to "live" state
    ↓
Stop I/O and cleanup
```

## Integration with NVME_CTRL_MARGINAL

This test exercises the complete call graph documented in the analysis:

1. **FC Transport Layer**: Manipulates `/sys/class/fc_remote_ports/rport-*/port_state`
2. **NVMe-FC Layer**: fcloop detects state changes and calls `nvme_fc_modify_rport_fpin_state()`
3. **NVMe Core**: Sets/clears `NVME_CTRL_MARGINAL` flag via `set_bit()/clear_bit()`
4. **Multipath Layer**: Path selection algorithms check `nvme_ctrl_is_marginal()`
5. **User Visibility**: State visible via `/sys/class/nvme/nvmeX/state`

## Expected Behavior

- **Normal → Marginal**: Controller should be deprioritized in multipath selection
- **Marginal → Normal**: Controller should return to normal multipath rotation
- **I/O Continuity**: Background I/O should continue (possibly with degraded performance)
- **State Visibility**: Controller state should be visible via sysfs

## Requirements

- FC transport support (`nvme-fc`, `nvme-fcloop` modules)
- NVMe target support (`nvmet` module)
- fio for I/O testing
- Root privileges for sysfs manipulation

## Files Created

- `tests/nvme/069` - Main test script
- `tests/nvme/069.out` - Expected output template
- `NVMe_MARGINAL_Test_Summary.md` - This documentation

## Usage

```bash
cd /path/to/blktests
# Run FC-specific test
NVMET_TRTYPES=fc ./check --cmd-trace tests/nvme/069
grep -e _set_attr results/nodev_tr_fc/nvme/069.cmdtrace

# Or run all FC tests including this one
NVMET_TRTYPES=fc ./check tests/nvme/
```

## Test Validation

The test validates:
1. ✅ FC remote port state manipulation via sysfs
2. ✅ NVMe controller marginal state detection
3. ✅ State transition monitoring (Online → Marginal → Online)
4. ✅ I/O behavior under marginal conditions
5. ✅ Recovery path functionality

This test provides comprehensive validation of the NVME_CTRL_MARGINAL flag functionality in a controlled, reproducible environment using the fcloop synthetic FC transport.
