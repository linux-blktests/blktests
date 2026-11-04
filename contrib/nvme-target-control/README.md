# nvme-target-control

Sets up and tears down remote NVMe targets for blktests. blktests calls it
through `NVME_TARGET_CONTROL` to run the fabrics tests against a target on
another machine instead of the local nvmet target, see
Documentation/running-tests.md.

It needs Python 3.10 or newer, and tomli on Python 3.10. It runs from the
source tree.

## Usage

    NVME_TARGET_CONTROL=$PWD/contrib/nvme-target-control/nvme-target-control \
        ./check nvme

Tests which change the local nvmet configfs, e.g. `attr_qid_max`, skip with
a remote target.

## Configuration

`~/.config/blktests/nvme_target_control.toml`, or the file
`NVME_TARGET_CONTROL_CONFIG` points to. It only says how to reach the
target, e.g. a Linux box with nvmet:

    [target]
    remote = "root@10.0.0.1"

The rest is worked out on the host or created on the target:

| Value | Default |
| --- | --- |
| `traddr` | the address of the target, e.g. of the ssh destination |
| `host_traddr` | the local address of the route to `traddr` |
| `hostnqn`, `hostid` | `/etc/nvme/hostnqn` and `/etc/nvme/hostid`, else the blktests defaults |
| `trtype` | `tcp` |
| `trsvid` | `4420` |
| `subsysnqn`, `subsys_uuid` | the blktests defaults |
| `blkdev_type` | `device` |

These are the values blktests reads with `config --show-*`. Each can be
set in `[target]`, e.g. `traddr` if the data path uses another address than
the ssh login. With `skip_setup_cleanup = true`, setup and cleanup do
nothing, to run against a target which is configured already.

## Targets

`type` in `[target]` selects the target:

| Type | Target |
| --- | --- |
| `nvmet` (default) | Linux nvmet target, configured through configfs |
| `package.module:Class` | A `Target` subclass outside this package |

### nvmet

Runs the shell script `nvmet.sh` on the target through a channel. Setup
creates the subsystem with namespace 1, allows the host and exports the
subsystem on new ports, one unless blktests asks for more. Unless `blkdev`
is set, the namespace is a loop device on an image file in
`/var/tmp/nvme-target-control` on the target. Cleanup removes the
subsystem, the ports which don't export another subsystem, the hosts no
other subsystem allows, and the loop device and image file.

A new port gets the lowest free nvmet port ID N, starting at 1, and listens
on `traddr` at service ID `trsvid` + N - 1. The first port of a target
without other ports listens on `trsvid`. The nvmet target implements all
port commands: `add-ports`, `add-port`, `remove-port`, `list-ports`,
`show-port` and `set-ana`, and `set-host`, which changes the DH-CHAP keys,
hash and DH group of the host.

| Option | Meaning |
| --- | --- |
| `channel` | `ssh` (default), `local` or `package.module:Class` |
| `remote` | ssh destination; the login must not ask for a password |
| `ssh_options` | list of extra ssh arguments |
| `blkdev` | block device or file backing the namespace, must keep the data |
| `img_size` | size of the image file without `blkdev`, default `1G` |

The target needs sshd, configfs, losetup and the nvmet modules for the
transport. The ssh destination may be an alias from the ssh config, `traddr`
is then its `HostName`.

## Adding a target

A target is a subclass of `nvme_target_control.target.Target`. It gets the
`[target]` section as `self.config`:

```python
from nvme_target_control.target import Subsystem, Target


class ArrayTarget(Target):
    def traddr(self) -> str:
        # The address the host connects to, without contacting the
        # target: blktests asks for it many times per test.
        return self.config['data_address']

    def setup(
        self, subsys: Subsystem, nr_ports: int = 1, tls: str = 'none'
    ) -> None:
        # Create subsys.nqn with one namespace, allow subsys.hostnqn and
        # export it on self.value('traddr'):self.value('trsvid').
        ...

    def cleanup(self, subsysnqn: str) -> None: ...
```

Select it with `type = "mymodule:ArrayTarget"`; `mymodule` must be
importable, e.g. through `PYTHONPATH`. It can override the other defaults
the same way, e.g. `subsysnqn` if the target names subsystems itself. A
target with a management API, e.g. a storage array with a REST API, talks
to it directly. A target configured with shell commands can use a channel:
`nvme_target_control.channel.create` creates the one `[target]` names. A
channel is a subclass of `Channel` which returns the command running
`sh -s -- ARGS` on the target and the host name of the target.

The port commands and `set-host` are optional. A target implements them
by overriding `add_ports`, `add_port`, `remove_port`, `list_ports`,
`show_port`, `set_ana` and `set_host`. `config --show-features` lists the
ones it overrides, followed by the class attribute `extra_features`, e.g.
`tls`. blktests skips the tests which need a feature the target doesn't
list. Without `list-ports` and `show-port`, blktests connects to `traddr`
and `trsvid`.

## Development

    ruff check . && ruff format --check . && mypy
    python3 -m unittest discover -s tests
    shellcheck -s sh nvme_target_control/targets/*.sh
