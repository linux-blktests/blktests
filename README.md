# blktests

[![Build Status](https://github.com/linux-blktests/blktests/workflows/CI/badge.svg)](https://github.com/linux-blktests/blktests/actions)

blktests is a test framework for the Linux kernel block layer and storage
stack. It is inspired by the [xfstests](https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git/)
filesystem testing framework. It was originally written by Omar Sandoval and
[announced in
2017](https://lore.kernel.org/linux-block/20170512184905.GA15267@vader.DHCP.thefacebook.com/).

## Getting Started

The dependencies are minimal, but make sure you have them installed:

- bash (>= 4.3)
- GNU coreutils
- GNU awk
- util-linux
- fio
- gcc
- make
- getconf
- systemd-udev (udevadm)

Some tests require the following:

- e2fsprogs, xfsprogs, f2fs-tools and btrfs-progs
- nvme-cli
- multipath-tools (Debian, openSUSE, Arch Linux) or device-mapper-multipath
  (Fedora)
- nbd-client and nbd-server (Debian) or nbd (Fedora, openSUSE, Arch Linux)
- dmsetup (Debian) or device-mapper (Fedora, openSUSE, Arch Linux)
- rublk (`cargo install --version=^0.1 rublk`) for ublk test
- python3, ethtool, iproute2 for nvme-tcp zero-copy offload test

Build blktests with `make`. Optionally, install it to a known location with
`make install` (`/usr/local/blktests` by default, but this can be changed by
passing `DESTDIR` and/or `prefix`).

Add the list of block devices you want to test on in a file named `config`
(**note:** these tests are potentially destructive):

```sh
TEST_DEVS=(/dev/nvme0n1 /dev/sdb)
```

And as root, run the default set of tests with `./check`.

**Do not** add anything to the `TEST_DEVS` array containing data that you want
to keep.

See [here](Documentation/running-tests.md) for more detailed information on
configuration and running tests.

## Adding Tests

New test cases are welcomed when,

- a bug in block layer or storage stack is found and the new test case confirms
  fix of the bug,
- a new feature is introduced in block layer or storage stack, and the new test
  cases confirm that the feature is working well, or,
- the new test cases extend coverage of block layer and storage stack code.

The `./new` script creates a new test from a template. The generated template
contains more detailed documentation. [The ./new script itself](new) can be
referred to as a document. It describes variables and functions that test cases
should implement, global variables that test cases can refer and coding
guidelines.

Patches to <linux-block@vger.kernel.org> and pull requests on GitHub are both
accepted. See [here](CONTRIBUTING.md) for more information on contributing.
