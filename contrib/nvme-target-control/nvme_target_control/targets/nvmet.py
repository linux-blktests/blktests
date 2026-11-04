# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""The Linux nvmet target, configured through configfs.

The configfs changes are in the shell script nvmet.sh next to this file.
It runs on the target through a channel, ssh by default. [target] options:

  channel      how to reach the target: 'ssh' (default), 'local' or
               'package.module:Class'
  remote       ssh destination, e.g. 'root@nvmet.local'
  ssh_options  list of extra ssh arguments
  blkdev       block device or file backing the namespace; by default
               setup creates a loop device on an image file on the target
               and cleanup removes both
  img_size     size of the image file, default 1G

A new port gets the lowest free port ID N >= 1 and the service ID
trsvid + N - 1, so the first port listens on trsvid. The values of
'config --show-*' can be set too, see Target.
"""

from collections.abc import Sequence
from pathlib import Path
from typing import Any

from .. import channel
from ..target import Port, Subsystem, Target, resolve

SCRIPT = Path(__file__).resolve().parent / 'nvmet.sh'

# The configfs attributes of the HOST_ATTRS
HOST_ATTRS = {
    'hostkey': 'dhchap_key',
    'ctrlkey': 'dhchap_ctrl_key',
    'hash': 'dhchap_hash',
    'dhgroup': 'dhchap_dhgroup',
}


class NvmetTarget(Target):
    """Linux nvmet target."""

    def __init__(self, config: dict[str, Any]) -> None:
        super().__init__(config)
        self.channel = channel.create(config)

    def traddr(self) -> str:
        return resolve(self.channel.host())

    def _run(self, *args: str) -> list[str]:
        """Run an nvmet.sh command, return the words it printed."""
        return self.channel.run(SCRIPT.read_text(), args).split()

    def _port_args(self, tls: str) -> Sequence[str]:
        return (
            self.value('trtype'),
            self.value('traddr'),
            self.value('trsvid'),
            tls,
        )

    def setup(
        self, subsys: Subsystem, nr_ports: int = 1, tls: str = 'none'
    ) -> None:
        self._run(
            'setup',
            *self._port_args(tls),
            str(nr_ports),
            subsys.nqn,
            subsys.uuid,
            str(self.config.get('blkdev', '')),
            str(self.config.get('img_size', '1G')),
            subsys.hostnqn,
            subsys.hostkey,
            subsys.ctrlkey,
        )

    def cleanup(self, subsysnqn: str) -> None:
        self._run('cleanup', subsysnqn)

    def add_ports(
        self, subsysnqn: str, nr_ports: int, tls: str = 'none'
    ) -> list[str]:
        return self._run(
            'add-ports', *self._port_args(tls), str(nr_ports), subsysnqn
        )

    def add_port(self, tls: str = 'none') -> str:
        (port,) = self._run('add-port', *self._port_args(tls))
        return port

    def remove_port(self, port: str) -> None:
        self._run('remove-port', port)

    def list_ports(self, subsysnqn: str) -> list[str]:
        return self._run('list-ports', subsysnqn)

    def show_port(self, port: str) -> Port:
        trtype, traddr, trsvcid = self._run('show-port', port)
        return Port(trtype, traddr, trsvcid)

    def set_ana(self, port: str, grpid: int, state: str) -> None:
        self._run('set-ana', port, str(grpid), state)

    def set_host(self, hostnqn: str, attrs: dict[str, str]) -> None:
        for name, value in attrs.items():
            self._run('set-host', hostnqn, HOST_ATTRS[name], value)
