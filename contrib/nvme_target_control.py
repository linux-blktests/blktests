#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+

# blktests calls this script to setup/teardown remote targets. blktests passes
# all relevant information via the command line, e.g. --hostnqn.
#
# This script uses nvmetcli to setup the remote target (it depends on the REST
# API feature [1]). There is not technical need for nvmetcli to use but it makes
# it simple to setup a remote Linux box. If you want to setup someting else
# you should to replace this part.
#
# There are couple of global configuration options which need to be set.
# Add ~/.config/blktests/nvme_target_control.toml file with something like:
#
# [main]
# skip_setup_cleanup=false
# nvmetcli='/usr/bin/nvmetcli'
# remote='http://nvmet.local:5000'
#
# [host]
# blkdev_type='device'
# trtype='tcp'
# hostnqn='nqn.2014-08.org.nvmexpress:uuid:0f01fb42-9f7f-4856-b0b3-51e60b8de349'
# hostid='0f01fb42-9f7f-4856-b0b3-51e60b8de349'
# host_traddr='192.168.154.187'
#
# [subsys_0]
# traddr='192.168.19.189'
# trsvid='4420'
# subsysnqn='blktests-subsystem-1'
# subsys_uuid='91fdba0d-f87b-4c25-b80f-db7be1418b9e'
#
# This expects nvmetcli with the restapi service running on target.
#
# Alternatively, you can skip the the target setup/cleanup completely
# (skip_setup_cleanup) and run against a previously configured target.
#
# [main]
# skip_setup_cleanup=true
# nvmetcli='/usr/bin/nvmetcli'
# remote='http://nvmet.local:5000'
#
# [host]
# blkdev_type='device'
# trtype='tcp'
# hostnqn='nqn.2014-08.org.nvmexpress:uuid:1a9e23dd-466e-45ca-9f43-a29aaf47cb21'
# hostid='1a9e23dd-466e-45ca-9f43-a29aaf47cb21'
# host_traddr='10.161.16.48'
#
# [subsys_0]
# traddr='10.162.198.45'
# trsvid='4420'
# subsysnqn='nqn.1988-11.com.dell:powerstore:00:f03028e73ef7D032D81E'
# subsys_uuid='3a5c104c-ee41-38a1-8ccf-0968003d54e7'
# blkdev='/dev/nullb0'
#
# nvmetcli uses JSON configuration, thus this script creates a JSON configuration
# using a jinja2 template. After this step we simple have to set the blktests
# variable correctly and start blktests.
#
#   NVME_TARGET_CONTROL=~/blktests/contrib/nvme_target_control.py ./check nvme
#
# [1] https://github.com/hreinecke/nvmetcli/tree/restapi

import os

# workaround for python<3.11
TOML_OPEN_MODE="rb"
try:
    import tomllib
except ModuleNotFoundError:
    import pip._vendor.tomli as tomllib
    TOML_OPEN_MODE="r"

import argparse
import subprocess
from jinja2 import Environment, FileSystemLoader


XDG_CONFIG_HOME = os.environ.get("XDG_CONFIG_HOME")
if not XDG_CONFIG_HOME:
    XDG_CONFIG_HOME = os.environ.get('HOME') + '/.config'


with open(f'{XDG_CONFIG_HOME}/blktests/nvme_target_control.toml', TOML_OPEN_MODE) as f:
    config = tomllib.load(f)
    nvmetcli = config['main']['nvmetcli']
    remote = config['main']['remote']


def gen_conf(conf):
    basepath = os.path.dirname(__file__)
    environment = Environment(loader=FileSystemLoader(basepath))
    template = environment.get_template('nvmet-subsys.jinja2')
    filename = f'{conf["subsysnqn"]}.json'
    content = template.render(conf)
    with open(filename, mode='w', encoding='utf-8') as outfile:
        outfile.write(content)


def target_setup(args):
    if config['main']['skip_setup_cleanup']:
        return

    conf = {
        'subsysnqn': args.subsysnqn,
        'subsys_uuid': args.subsys_uuid,
        'hostnqn': args.hostnqn,
        'allowed_hosts': args.hostnqn,
        'ctrlkey': args.ctrlkey,
        'hostkey': args.hostkey,
        'blkdev': config['subsys_0']['blkdev'],
    }

    gen_conf(conf)

    subprocess.call(['python3', nvmetcli, '--remote=' + remote,
                     'restore', args.subsysnqn + '.json'])

def target_add_ports(_):
    if config['main']['skip_setup_cleanup']:
        return

    print("Warning: add-ports is not implemented yet.")

def target_cleanup(args):
    if config['main']['skip_setup_cleanup']:
        return

    subprocess.call(['python3', nvmetcli, '--remote=' + remote,
                     'clear', args.subsysnqn + '.json'])


def target_config(args):
	if args.show_blkdev_type:
		print(config['host']['blkdev_type'])
	elif args.show_trtype:
		print(config['host']['trtype'])
	elif args.show_hostnqn:
		print(config['host']['hostnqn'])
	elif args.show_hostid:
		print(config['host']['hostid'])
	elif args.show_host_traddr:
		print(config['host']['host_traddr'])
	elif args.show_traddr:
		print(config['subsys_0']['traddr'])
	elif args.show_trsvid:
		print(config['subsys_0']['trsvid'])
	elif args.show_subsysnqn:
		print(config['subsys_0']['subsysnqn'])
	elif args.show_subsys_uuid:
		print(config['subsys_0']['subsys_uuid'])


def build_parser():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(required=True)

    setup = sub.add_parser('setup')
    setup.add_argument('--subsysnqn', required=True)
    setup.add_argument('--subsys-uuid', required=True)
    setup.add_argument('--hostnqn', required=True)
    setup.add_argument('--ctrlkey', default='')
    setup.add_argument('--hostkey', default='')
    setup.set_defaults(func=target_setup)

    setup = sub.add_parser('add-ports')
    setup.add_argument('--ports', required=True)
    setup.add_argument('--subsysnqn', required=True)
    setup.add_argument('--subsys-uuid', required=True)
    setup.add_argument('--hostnqn', required=True)
    setup.add_argument('--ctrlkey', default='')
    setup.add_argument('--hostkey', default='')
    setup.set_defaults(func=target_add_ports)

    cleanup = sub.add_parser('cleanup')
    cleanup.add_argument('--subsysnqn', required=True)
    cleanup.set_defaults(func=target_cleanup)

    config = sub.add_parser('config')
    config.add_argument('--show-blkdev-type', action='store_true')
    config.add_argument('--show-trtype', action='store_true')
    config.add_argument('--show-hostnqn', action='store_true')
    config.add_argument('--show-hostid', action='store_true')
    config.add_argument('--show-host-traddr', action='store_true')
    config.add_argument('--show-traddr', action='store_true')
    config.add_argument('--show-trsvid', action='store_true')
    config.add_argument('--show-subsys-uuid', action='store_true')
    config.add_argument('--show-subsysnqn', action='store_true')
    config.set_defaults(func=target_config)

    return parser


def main():
    import sys

    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
