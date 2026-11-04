# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
"""Tests for the command line, the plugin lookup and the nvmet target."""

import contextlib
import io
import os
import shlex
import sys
import tempfile
import types
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any, ClassVar
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from nvme_target_control import channel, cli, plugin, target
from nvme_target_control.targets import nvmet

CONFIG = """\
[target]
remote = "root@127.0.0.1"
"""

SETUP_ARGS = [
    'setup',
    '--subsysnqn', 'blktests-subsystem-1',
    '--subsys-uuid', '91fdba0d-f87b-4c25-b80f-db7be1418b9e',
    '--hostnqn', 'nqn.2014-08.org.nvmexpress:uuid:57c34e91',
]  # fmt: skip


class RecordingTarget(target.Target):
    """A target plugin which records the calls."""

    calls: ClassVar[list[tuple[str, Any]]] = []

    def traddr(self) -> str:
        return '192.0.2.1'

    def setup(
        self, subsys: target.Subsystem, nr_ports: int = 1, tls: str = 'none'
    ) -> None:
        self.calls.append(('setup', (subsys, nr_ports, tls)))

    def cleanup(self, subsysnqn: str) -> None:
        self.calls.append(('cleanup', subsysnqn))


PLUGIN = """\
[target]
type = "fake_array:RecordingTarget"
"""


class ConfigTestCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.config = Path(tmp.name) / 'config.toml'
        self.config.write_text(CONFIG)
        env = mock.patch.dict(
            os.environ, {'NVME_TARGET_CONTROL_CONFIG': str(self.config)}
        )
        env.start()
        self.addCleanup(env.stop)
        module = types.ModuleType('fake_array')
        module.RecordingTarget = RecordingTarget  # type: ignore[attr-defined]
        modules = mock.patch.dict(sys.modules, {'fake_array': module})
        modules.start()
        self.addCleanup(modules.stop)
        RecordingTarget.calls = []

    def main(self, argv: Sequence[str]) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with (
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            ret = cli.main(argv)
        return ret, out.getvalue(), err.getvalue()

    def show(self, option: str) -> str:
        ret, out, err = self.main(['config', f'--show-{option}'])
        self.assertEqual((ret, err), (0, ''))
        return out.rstrip('\n')


class TestCli(ConfigTestCase):
    def test_defaults(self) -> None:
        self.assertEqual(self.show('traddr'), '127.0.0.1')
        self.assertEqual(self.show('host-traddr'), '127.0.0.1')
        self.assertEqual(self.show('trtype'), 'tcp')
        self.assertEqual(self.show('trsvid'), '4420')
        self.assertEqual(self.show('subsysnqn'), 'blktests-subsystem-1')
        self.assertEqual(
            self.show('subsys-uuid'), '91fdba0d-f87b-4c25-b80f-db7be1418b9e'
        )
        self.assertEqual(self.show('blkdev-type'), 'device')

    def test_host_identity(self) -> None:
        files = {
            '/etc/nvme/hostid': 'abc',
            '/etc/nvme/hostnqn': 'nqn.test:host',
        }
        with mock.patch.object(target, 'read_first_line', files.get):
            self.assertEqual(self.show('hostid'), 'abc')
            self.assertEqual(self.show('hostnqn'), 'nqn.test:host')
        with mock.patch.object(target, 'read_first_line', lambda _: None):
            self.assertEqual(self.show('hostid'), target.DEF_HOSTID)
            self.assertEqual(
                self.show('hostnqn'),
                f'nqn.2014-08.org.nvmexpress:uuid:{target.DEF_HOSTID}',
            )

    def test_overrides(self) -> None:
        self.config.write_text(
            CONFIG + 'traddr = "10.9.9.9"\ntrsvid = 4421\nhostid = "x"\n'
        )
        self.assertEqual(self.show('traddr'), '10.9.9.9')
        self.assertEqual(self.show('trsvid'), '4421')
        self.assertEqual(self.show('hostid'), 'x')

    def test_external_target_plugin(self) -> None:
        self.config.write_text(PLUGIN)
        self.assertEqual(self.show('traddr'), '192.0.2.1')
        ret, _, err = self.main([*SETUP_ARGS, '--ctrlkey', 'ckey'])
        self.assertEqual((ret, err), (0, ''))
        ret, _, err = self.main(
            ['cleanup', '--subsysnqn', 'blktests-subsystem-1']
        )
        self.assertEqual((ret, err), (0, ''))
        (cmd, (subsys, nr_ports, tls)), (cmd2, nqn) = RecordingTarget.calls
        self.assertEqual(cmd, 'setup')
        self.assertEqual(subsys.nqn, 'blktests-subsystem-1')
        self.assertEqual(subsys.ctrlkey, 'ckey')
        self.assertEqual((nr_ports, tls), (1, 'none'))
        self.assertEqual((cmd2, nqn), ('cleanup', 'blktests-subsystem-1'))

    def test_setup_ports_tls(self) -> None:
        self.config.write_text(PLUGIN)
        ret, _, err = self.main([*SETUP_ARGS, '--ports', '3', '--tls'])
        self.assertEqual((ret, err), (0, ''))
        ret, _, err = self.main([*SETUP_ARGS, '--force-tls'])
        self.assertEqual((ret, err), (0, ''))
        (_, (_, n1, tls1)), (_, (_, n2, tls2)) = RecordingTarget.calls
        self.assertEqual(
            (n1, tls1, n2, tls2), (3, 'not-required', 1, 'required')
        )
        with (
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            cli.main([*SETUP_ARGS, '--tls', '--force-tls'])

    def test_features(self) -> None:
        self.assertEqual(
            self.show('features'),
            'add-ports add-port remove-port list-ports show-port set-ana '
            'set-host',
        )
        self.config.write_text(PLUGIN)
        self.assertEqual(self.show('features'), '')

    def test_set_host(self) -> None:
        with mock.patch.object(nvmet.NvmetTarget, 'set_host') as set_host:
            ret, _, err = self.main(
                ['set-host', '--hostnqn', 'nqn.h', '--dhgroup', 'ffdhe2048']
            )
        self.assertEqual((ret, err), (0, ''))
        set_host.assert_called_once_with('nqn.h', {'dhgroup': 'ffdhe2048'})

    def test_port_output(self) -> None:
        with (
            mock.patch.object(
                nvmet.NvmetTarget,
                'show_port',
                return_value=target.Port('tcp', '10.0.0.1', '4421'),
            ),
            mock.patch.object(
                nvmet.NvmetTarget, 'add_ports', return_value=['2', '3']
            ),
        ):
            ret, out, _ = self.main(['show-port', '--port', '2'])
            self.assertEqual((ret, out), (0, 'tcp 10.0.0.1 4421\n'))
            ret, out, _ = self.main(
                ['add-ports', '--ports', '2', '--subsysnqn', 'nqn.s']
            )
            self.assertEqual((ret, out), (0, '2\n3\n'))

    def test_ports_not_implemented(self) -> None:
        self.config.write_text(PLUGIN)
        for argv in [
            ['add-ports', '--ports', '2', *SETUP_ARGS[1:]],
            ['add-port'],
            ['remove-port', '--port', '1'],
            ['list-ports', '--subsysnqn', 'nqn.s'],
            ['show-port', '--port', '1'],
            ['set-ana', '--port', '1', '--grpid', '1', '--state', 'optimized'],
            ['set-host', '--hostnqn', 'nqn.h', '--hash', 'hmac(sha256)'],
        ]:
            ret, out, err = self.main(argv)
            self.assertEqual((ret, out), (1, ''))
            self.assertIn(f'does not implement {argv[0]}', err)

    def test_skip_setup_cleanup(self) -> None:
        self.config.write_text(PLUGIN + 'skip_setup_cleanup = true\n')
        ret, _, err = self.main(SETUP_ARGS)
        self.assertEqual((ret, err), (0, ''))
        self.assertEqual(RecordingTarget.calls, [])

    def test_unknown_target(self) -> None:
        self.config.write_text('[target]\ntype = "foo"\n')
        ret, _, err = self.main(SETUP_ARGS)
        self.assertEqual(ret, 1)
        self.assertIn("unknown target 'foo'", err)

    def test_missing_config(self) -> None:
        self.config.unlink()
        ret, _, err = self.main(SETUP_ARGS)
        self.assertEqual(ret, 1)
        self.assertIn('FileNotFoundError', err)


class TestPlugin(unittest.TestCase):
    def test_builtin_and_path(self) -> None:
        self.assertIs(
            plugin.load('channel', 'ssh', channel.CHANNELS),
            channel.SshChannel,
        )
        self.assertIs(
            plugin.load(
                'channel',
                'nvme_target_control.channel:LocalChannel',
                channel.CHANNELS,
            ),
            channel.LocalChannel,
        )

    def test_errors(self) -> None:
        for name in ['foo', 'no_such_module:Foo', 'nvme_target_control:Foo']:
            with self.assertRaises(plugin.PluginError):
                plugin.load('channel', name, channel.CHANNELS)

    def test_wrong_base_class(self) -> None:
        with self.assertRaises(plugin.PluginError):
            channel.create({'channel': 'nvme_target_control.target:Target'})


class TestChannel(unittest.TestCase):
    def test_ssh_command(self) -> None:
        ch = channel.SshChannel(
            {'remote': 'root@t', 'ssh_options': ['-p', '22']}
        )
        cmd = ch.command(['nqn with space', "it's", ''])
        self.assertEqual(cmd[:4], ['ssh', '-p', '22', 'root@t'])
        # The remote shell splits the command back into the arguments.
        self.assertEqual(
            shlex.split(cmd[4]),
            ['sh', '-s', '--', 'nqn with space', "it's", ''],
        )

    def test_ssh_host(self) -> None:
        ch = channel.SshChannel({'remote': 'root@192.0.2.7'})
        self.assertEqual(ch.host(), '192.0.2.7')

    def test_ssh_needs_remote(self) -> None:
        with self.assertRaises(channel.ChannelError):
            channel.SshChannel({}).command([])

    def test_local_run(self) -> None:
        out = channel.LocalChannel({}).run('printf "%s|" "$@"', ['a b', ''])
        self.assertEqual(out, 'a b||')

    def test_local_run_fails(self) -> None:
        with self.assertRaises(channel.ChannelError):
            channel.LocalChannel({}).run('exit 3', [])


class FakeChannel(channel.Channel):
    runs: ClassVar[list[tuple[str, list[str]]]] = []

    def command(self, args: Sequence[str]) -> list[str]:
        return []

    def host(self) -> str:
        return '192.0.2.5'

    output: ClassVar[str] = ''

    def run(self, script: str, args: Sequence[str]) -> str:
        self.runs.append((script, list(args)))
        return self.output


class TestNvmet(unittest.TestCase):
    def target(
        self, config: dict[str, Any], output: str = ''
    ) -> nvmet.NvmetTarget:
        FakeChannel.runs = []
        FakeChannel.output = output
        return nvmet.NvmetTarget(
            {'channel': f'{__name__}:FakeChannel', **config}
        )

    def test_traddr_from_channel(self) -> None:
        self.assertEqual(self.target({}).value('traddr'), '192.0.2.5')

    def test_setup(self) -> None:
        tgt = self.target({})
        tgt.setup(target.Subsystem('nqn.s', 'uuid', 'nqn.h', 'hk', 'ck'))
        ((script, args),) = FakeChannel.runs
        self.assertIn('/sys/kernel/config/nvmet', script)
        self.assertEqual(
            args,
            ['setup', 'tcp', '192.0.2.5', '4420', 'none', '1', 'nqn.s',
             'uuid', '', '1G', 'nqn.h', 'hk', 'ck'],
        )  # fmt: skip

    def test_setup_options(self) -> None:
        tgt = self.target(
            {'blkdev': '/dev/nullb0', 'traddr': '10.0.0.1', 'trsvid': 4430}
        )
        tgt.setup(target.Subsystem('nqn.s', 'uuid', 'nqn.h'), 2, 'required')
        ((_, args),) = FakeChannel.runs
        self.assertEqual(
            args[2:6] + args[8:9],
            ['10.0.0.1', '4430', 'required', '2', '/dev/nullb0'],
        )

    def test_cleanup(self) -> None:
        tgt = self.target({})
        tgt.cleanup('nqn.s')
        ((script, args),) = FakeChannel.runs
        self.assertIn('losetup -d', script)
        self.assertEqual(args, ['cleanup', 'nqn.s'])

    def test_ports(self) -> None:
        port_args = ['tcp', '192.0.2.5', '4420', 'none']
        tgt = self.target({}, '2\n3\n')
        self.assertEqual(tgt.add_ports('nqn.s', 2), ['2', '3'])
        self.assertEqual(tgt.list_ports('nqn.s'), ['2', '3'])
        self.assertEqual(
            [args for _, args in FakeChannel.runs],
            [['add-ports', *port_args, '2', 'nqn.s'],
             ['list-ports', 'nqn.s']],
        )  # fmt: skip

        tgt = self.target({}, '4\n')
        self.assertEqual(tgt.add_port('not-required'), '4')
        tgt.remove_port('4')
        tgt.set_ana('4', 2, 'inaccessible')
        self.assertEqual(
            [args for _, args in FakeChannel.runs],
            [['add-port', 'tcp', '192.0.2.5', '4420', 'not-required'],
             ['remove-port', '4'],
             ['set-ana', '4', '2', 'inaccessible']],
        )  # fmt: skip

        tgt = self.target({})
        tgt.set_host(
            'nqn.h', {'ctrlkey': 'DHHC-1:00:x:', 'hash': 'hmac(sha512)'}
        )
        self.assertEqual(
            [args for _, args in FakeChannel.runs],
            [['set-host', 'nqn.h', 'dhchap_ctrl_key', 'DHHC-1:00:x:'],
             ['set-host', 'nqn.h', 'dhchap_hash', 'hmac(sha512)']],
        )  # fmt: skip

        tgt = self.target({}, 'tcp 10.0.0.1 4421\n')
        self.assertEqual(
            tgt.show_port('2'), target.Port('tcp', '10.0.0.1', '4421')
        )


if __name__ == '__main__':
    unittest.main()
