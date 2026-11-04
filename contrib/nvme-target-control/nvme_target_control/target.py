# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""Targets configure a subsystem on a kind of NVMe target.

A target implementation gets the [target] section of the configuration. It
answers the values blktests asks for with 'config --show-*' and creates the
subsystem blktests connects to. The built-in 'nvmet' target configures a
Linux nvmet target. Other targets, e.g. a storage array with a management
API, can be added as 'package.module:Class' in the [target] 'type' option.
"""

import socket
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, ClassVar

from . import plugin

# The defaults of blktests, see common/nvme
DEF_HOSTID = '0f01fb42-9f7f-4856-b0b3-51e60b8de349'
DEF_SUBSYSNQN = 'blktests-subsystem-1'
DEF_SUBSYS_UUID = '91fdba0d-f87b-4c25-b80f-db7be1418b9e'
DEF_TRSVCID = '4420'


@dataclass(frozen=True)
class Subsystem:
    """The subsystem blktests wants to connect to."""

    nqn: str
    uuid: str
    hostnqn: str
    hostkey: str = ''
    ctrlkey: str = ''


@dataclass(frozen=True)
class Port:
    """The address of a port, what the host passes to nvme connect."""

    trtype: str
    traddr: str
    trsvcid: str


# The TLS modes of a port: no TLS, TLS optional, TLS required
TLS_MODES = ('none', 'not-required', 'required')

# The DH-CHAP attributes of a host: the host key, the controller key for
# bidirectional authentication, the hash and the DH group, e.g.
# 'hmac(sha256)' and 'ffdhe2048'
HOST_ATTRS = ('hostkey', 'ctrlkey', 'hash', 'dhgroup')


def read_first_line(path: str) -> str | None:
    try:
        return Path(path).read_text().split('\n', 1)[0].strip() or None
    except OSError:
        return None


def resolve(host: str) -> str:
    """Return the IP address of host."""
    return str(
        socket.getaddrinfo(host, None, type=socket.SOCK_STREAM)[0][4][0]
    )


def source_address(addr: str) -> str:
    """Return the local address of the route to addr."""
    family = socket.AF_INET6 if ':' in addr else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as s:
        # Connecting a UDP socket sends nothing, it only picks the route.
        s.connect((addr, 9))
        return str(s.getsockname()[0])


class Target(ABC):
    """A kind of NVMe target.

    config is the [target] section. Each value blktests asks for can be set
    there with the name of the 'config --show-*' option, e.g. 'traddr'.
    Otherwise the target works it out, see the methods below. They must not
    contact the target: blktests asks for them many times per test.
    """

    def __init__(self, config: dict[str, Any]) -> None:
        self.config = config

    def value(self, name: str) -> str:
        """Return the value of 'config --show-NAME'."""
        if name in self.config:
            return str(self.config[name])
        return str(getattr(self, name)())

    def trtype(self) -> str:
        return 'tcp'

    @abstractmethod
    def traddr(self) -> str:
        """Return the address the host connects to."""

    def trsvid(self) -> str:
        return DEF_TRSVCID

    def host_traddr(self) -> str:
        return source_address(self.value('traddr'))

    def hostnqn(self) -> str:
        return (
            read_first_line('/etc/nvme/hostnqn')
            or f'nqn.2014-08.org.nvmexpress:uuid:{self.value("hostid")}'
        )

    def hostid(self) -> str:
        return read_first_line('/etc/nvme/hostid') or DEF_HOSTID

    def subsysnqn(self) -> str:
        return DEF_SUBSYSNQN

    def subsys_uuid(self) -> str:
        return DEF_SUBSYS_UUID

    def blkdev_type(self) -> str:
        return 'device'

    @abstractmethod
    def setup(
        self, subsys: Subsystem, nr_ports: int = 1, tls: str = 'none'
    ) -> None:
        """Create subsys with one namespace and export it on new ports.

        tls is one of TLS_MODES.
        """

    @abstractmethod
    def cleanup(self, subsysnqn: str) -> None:
        """Remove the subsystem and whatever setup created for it."""

    # The optional commands.  Ports are named by IDs the target picks.  A
    # target leaves the commands it can't do unimplemented.  blktests
    # skips the tests which need a feature 'config --show-features'
    # doesn't list.  Without list-ports and show-port, it connects to
    # traddr and trsvid.

    # Features which are not commands, e.g. 'tls'
    extra_features: ClassVar[tuple[str, ...]] = ()

    def features(self) -> list[str]:
        """Return the optional commands the target implements.

        Followed by extra_features.
        """
        return [
            cmd
            for cmd, method in OPTIONAL_COMMANDS.items()
            if getattr(type(self), method) is not getattr(Target, method)
        ] + list(self.extra_features)

    def _not_implemented(self, what: str) -> NotImplementedError:
        return NotImplementedError(
            f'{type(self).__name__} does not implement {what}'
        )

    def add_ports(
        self, subsysnqn: str, nr_ports: int, tls: str = 'none'
    ) -> list[str]:
        """Export the subsystem on nr_ports new ports, return their IDs."""
        raise self._not_implemented('add-ports')

    def add_port(self, tls: str = 'none') -> str:
        """Create a port which exports no subsystem, return its ID."""
        raise self._not_implemented('add-port')

    def remove_port(self, port: str) -> None:
        """Remove the port."""
        raise self._not_implemented('remove-port')

    def list_ports(self, subsysnqn: str) -> list[str]:
        """Return the IDs of the ports which export the subsystem."""
        raise self._not_implemented('list-ports')

    def show_port(self, port: str) -> Port:
        """Return the address of the port."""
        raise self._not_implemented('show-port')

    def set_ana(self, port: str, grpid: int, state: str) -> None:
        """Set the state of ANA group grpid on the port."""
        raise self._not_implemented('set-ana')

    def set_host(self, hostnqn: str, attrs: dict[str, str]) -> None:
        """Change the DH-CHAP attributes of a host setup allowed.

        attrs maps names of HOST_ATTRS to their new values.
        """
        raise self._not_implemented('set-host')


# The optional commands and the Target methods implementing them
OPTIONAL_COMMANDS = {
    'add-ports': 'add_ports',
    'add-port': 'add_port',
    'remove-port': 'remove_port',
    'list-ports': 'list_ports',
    'show-port': 'show_port',
    'set-ana': 'set_ana',
    'set-host': 'set_host',
}

TARGETS = {
    'nvmet': 'nvme_target_control.targets.nvmet:NvmetTarget',
}


def create(config: dict[str, Any]) -> Target:
    """Create the target the [target] section describes."""
    cls = plugin.load('target', config.get('type', 'nvmet'), TARGETS)
    if not (isinstance(cls, type) and issubclass(cls, Target)):
        raise plugin.PluginError(f'{cls!r} is not a Target')
    return cls(config)
