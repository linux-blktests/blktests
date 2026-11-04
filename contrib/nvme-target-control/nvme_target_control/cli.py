# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""The command line interface blktests calls through NVME_TARGET_CONTROL."""

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from . import target
from .channel import ChannelError
from .plugin import PluginError

if sys.version_info >= (3, 11):
    import tomllib
else:
    import tomli as tomllib  # type: ignore[import-not-found,unused-ignore]


def config_path() -> Path:
    """Return the configuration file.

    NVME_TARGET_CONTROL_CONFIG overrides the default
    $XDG_CONFIG_HOME/blktests/nvme_target_control.toml.
    """
    path = os.environ.get('NVME_TARGET_CONTROL_CONFIG')
    if path:
        return Path(path)
    base = os.environ.get('XDG_CONFIG_HOME') or str(Path.home() / '.config')
    return Path(base) / 'blktests' / 'nvme_target_control.toml'


def load_config(path: Path) -> dict[str, Any]:
    with path.open('rb') as f:
        config: dict[str, Any] = tomllib.load(f)
    return config


# The config --show-* options, named like the Target methods
SHOW = [
    'blkdev_type',
    'trtype',
    'hostnqn',
    'hostid',
    'host_traddr',
    'traddr',
    'trsvid',
    'subsys_uuid',
    'subsysnqn',
]


def show(tgt: target.Target, args: argparse.Namespace) -> None:
    if args.show_features:
        print(' '.join(tgt.features()))
        return
    for option in SHOW:
        if getattr(args, f'show_{option}'):
            print(tgt.value(option))
            return


def subsystem(args: argparse.Namespace) -> target.Subsystem:
    return target.Subsystem(
        nqn=args.subsysnqn,
        uuid=args.subsys_uuid,
        hostnqn=args.hostnqn,
        hostkey=args.hostkey,
        ctrlkey=args.ctrlkey,
    )


def print_lines(lines: Sequence[str]) -> None:
    for line in lines:
        print(line)


def run(config: dict[str, Any], args: argparse.Namespace) -> None:
    tgt = target.create(config['target'])
    if args.cmd == 'config':
        show(tgt, args)
        return
    if config['target'].get('skip_setup_cleanup', False):
        return

    if args.cmd == 'setup':
        tgt.setup(subsystem(args), args.ports, args.tls)
    elif args.cmd == 'cleanup':
        tgt.cleanup(args.subsysnqn)
    elif args.cmd == 'add-ports':
        print_lines(tgt.add_ports(args.subsysnqn, args.ports, args.tls))
    elif args.cmd == 'add-port':
        print(tgt.add_port(args.tls))
    elif args.cmd == 'remove-port':
        tgt.remove_port(args.port)
    elif args.cmd == 'list-ports':
        print_lines(tgt.list_ports(args.subsysnqn))
    elif args.cmd == 'show-port':
        port = tgt.show_port(args.port)
        print(port.trtype, port.traddr, port.trsvcid)
    elif args.cmd == 'set-ana':
        tgt.set_ana(args.port, args.grpid, args.state)
    elif args.cmd == 'set-host':
        attrs = {
            name: getattr(args, name)
            for name in target.HOST_ATTRS
            if getattr(args, name) is not None
        }
        tgt.set_host(args.hostnqn, attrs)


def add_subsys_args(
    parser: argparse.ArgumentParser, required: bool = True
) -> None:
    parser.add_argument('--subsysnqn', required=True)
    parser.add_argument('--subsys-uuid', required=required)
    parser.add_argument('--hostnqn', required=required)
    parser.add_argument('--ctrlkey', default='')
    parser.add_argument('--hostkey', default='')


def add_port_args(parser: argparse.ArgumentParser, ports: bool) -> None:
    """Add the options of the commands which create ports."""
    if ports:
        parser.add_argument('--ports', type=int, default=1)
    tls = parser.add_mutually_exclusive_group()
    # The options of _nvmet_target_setup in common/nvme
    tls.add_argument(
        '--tls',
        action='store_const',
        dest='tls',
        const='not-required',
        default='none',
    )
    tls.add_argument(
        '--force-tls', action='store_const', dest='tls', const='required'
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog='nvme-target-control',
        description='Set up and tear down remote NVMe targets for blktests.',
    )
    sub = parser.add_subparsers(dest='cmd', required=True)

    setup = sub.add_parser('setup')
    add_subsys_args(setup)
    add_port_args(setup, ports=True)

    cleanup = sub.add_parser('cleanup')
    cleanup.add_argument('--subsysnqn', required=True)

    # common/nvme passes the setup options to add-ports, only the
    # subsystem NQN is used.
    add_ports = sub.add_parser('add-ports')
    add_subsys_args(add_ports, required=False)
    add_port_args(add_ports, ports=True)

    add_port_args(sub.add_parser('add-port'), ports=False)

    sub.add_parser('remove-port').add_argument('--port', required=True)

    sub.add_parser('list-ports').add_argument('--subsysnqn', required=True)

    sub.add_parser('show-port').add_argument('--port', required=True)

    set_ana = sub.add_parser('set-ana')
    set_ana.add_argument('--port', required=True)
    set_ana.add_argument('--grpid', type=int, required=True)
    set_ana.add_argument('--state', required=True)

    set_host = sub.add_parser('set-host')
    set_host.add_argument('--hostnqn', required=True)
    for name in target.HOST_ATTRS:
        set_host.add_argument('--' + name)

    cfg = sub.add_parser('config')
    for option in SHOW:
        cfg.add_argument(
            '--show-' + option.replace('_', '-'), action='store_true'
        )
    cfg.add_argument('--show-features', action='store_true')

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        run(load_config(config_path()), args)
    except (
        ChannelError,
        PluginError,
        NotImplementedError,
        KeyError,
        OSError,
        ValueError,
        tomllib.TOMLDecodeError,
    ) as e:
        print(f'nvme-target-control: {e!r}', file=sys.stderr)
        return 1
    return 0
