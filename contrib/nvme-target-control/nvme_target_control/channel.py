# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""Channels run shell scripts on the target.

Targets which are configured with shell commands, like the Linux nvmet
target, use a channel to reach the target machine. Targets with their own
management API don't need one.
"""

import shlex
import subprocess
from abc import ABC, abstractmethod
from collections.abc import Sequence
from typing import Any

from . import plugin


class ChannelError(Exception):
    """A script failed on the target."""


class Channel(ABC):
    """Runs shell scripts on the target."""

    def __init__(self, config: dict[str, Any]) -> None:
        self.config = config

    @abstractmethod
    def command(self, args: Sequence[str]) -> list[str]:
        """Return the command which runs 'sh -s -- args' on the target."""

    @abstractmethod
    def host(self) -> str:
        """Return the host name or address of the target machine."""

    def run(self, script: str, args: Sequence[str]) -> str:
        """Run script with the positional parameters args.

        Return what the script printed on stdout.  Its stderr is passed
        through.
        """
        cmd = self.command(args)
        ret = subprocess.run(
            cmd,
            input=script,
            stdout=subprocess.PIPE,
            text=True,
            check=False,
        )
        if ret.returncode:
            raise ChannelError(
                f'{shlex.join(cmd)} failed with exit code {ret.returncode}'
            )
        return ret.stdout


class LocalChannel(Channel):
    """Runs the scripts on this machine."""

    def command(self, args: Sequence[str]) -> list[str]:
        return ['sh', '-s', '--', *args]

    def host(self) -> str:
        return '127.0.0.1'


class SshChannel(Channel):
    """Runs the scripts on the 'remote' ssh destination.

    'ssh_options' are passed to ssh. The login must not ask for a password.
    """

    def _ssh(self) -> list[str]:
        try:
            remote = self.config['remote']
        except KeyError:
            raise ChannelError("ssh channel needs 'remote'") from None
        return ['ssh', *self.config.get('ssh_options', []), remote]

    def command(self, args: Sequence[str]) -> list[str]:
        # ssh passes the command to the remote shell as one string.
        return [*self._ssh(), shlex.join(['sh', '-s', '--', *args])]

    def host(self) -> str:
        # ssh -G prints the configuration for the destination without
        # connecting, including the host name of an alias.
        ssh = self._ssh()
        ret = subprocess.run(
            [ssh[0], '-G', *ssh[1:]],
            capture_output=True,
            text=True,
            check=False,
        )
        for line in ret.stdout.splitlines():
            key, _, value = line.partition(' ')
            if key == 'hostname':
                return value
        raise ChannelError(f'ssh -G {ssh[-1]} failed: {ret.stderr.strip()}')


CHANNELS = {
    'local': f'{__name__}:LocalChannel',
    'ssh': f'{__name__}:SshChannel',
}


def create(config: dict[str, Any]) -> Channel:
    """Create the channel named by config['channel'], ssh by default."""
    cls = plugin.load('channel', config.get('channel', 'ssh'), CHANNELS)
    if not (isinstance(cls, type) and issubclass(cls, Channel)):
        raise plugin.PluginError(f'{cls!r} is not a Channel')
    return cls(config)
