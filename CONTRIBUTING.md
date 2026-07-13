# Contributing to blktests

You can contribute to blktests by sending patches to the
<linux-block@vger.kernel.org> mailing list and Shin'ichiro Kawasaki <shinichiro.kawasaki@wdc.com>
or by opening a pull request to the [blktests GitHub
repository](https://github.com/linux-blktests/blktests). Patch post is more recommended
since it will be visible to more kernel developers and easier to gather
feedback. If sending patches, please generate the patch with `git format-patch
--subject-prefix="PATCH blktests"`. Consider configuring git to do this for you
with `git config --local format.subjectPrefix "PATCH blktests"`.

All commits must be signed off (i.e., `Signed-off-by: Jane Doe <janedoe@example.org>`)
as per the [Developer Certificate of Origin](https://developercertificate.org/).
`git commit -s` and `git format-patch -s` can do this for you.

Please run `make check` before submitting a new test. This runs the
[shellcheck](https://github.com/koalaman/shellcheck) static analysis tool and
some other sanity checks.

When you add new files, choose a license from GPL-3.0+ or GPL-2.0+. The main
license of blktests is GPL-3.0+. GPL-3.0+ is recommended. GPL-2.0+ is allowed
but GPL-2.0 cannot be applied.
