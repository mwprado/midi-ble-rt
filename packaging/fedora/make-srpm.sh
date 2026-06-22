#!/usr/bin/env bash
set -euo pipefail

# Create a source RPM from the current git checkout.
#
# Typical COPR flow:
#
#   packaging/fedora/make-srpm.sh
#   copr-cli build USER/PROJECT "$HOME/rpmbuild/SRPMS/midi-ble-rt-*.src.rpm"
#
# The spec lives in packaging/fedora and uses Source0 from the upstream tag.
# This helper generates a matching tarball from HEAD for local SRPM builds.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SPEC="$REPO_ROOT/packaging/fedora/midi-ble-rt.spec"

if [[ ! -f "$SPEC" ]]; then
    echo "error: spec not found: $SPEC" >&2
    exit 1
fi

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "error: rpmbuild not found" >&2
    echo "hint: sudo dnf install rpm-build rpmdevtools" >&2
    exit 1
fi

if ! command -v rpmspec >/dev/null 2>&1; then
    echo "error: rpmspec not found" >&2
    echo "hint: sudo dnf install rpm-build" >&2
    exit 1
fi

NAME="$(rpmspec -q --qf '%{name}\n' "$SPEC" | head -n1)"
VERSION="$(rpmspec -q --qf '%{version}\n' "$SPEC" | head -n1)"
TOPDIR="${RPM_TOPDIR:-$HOME/rpmbuild}"
SOURCES="$TOPDIR/SOURCES"
SRPMS="$TOPDIR/SRPMS"
TARBALL="$SOURCES/$NAME-$VERSION.tar.gz"

mkdir -p "$SOURCES" "$SRPMS" "$TOPDIR/BUILD" "$TOPDIR/BUILDROOT" "$TOPDIR/RPMS" "$TOPDIR/SPECS"

cd "$REPO_ROOT"

git diff --quiet || {
    echo "error: working tree has unstaged/uncommitted changes" >&2
    echo "hint: commit or stash before creating the SRPM" >&2
    exit 1
}

git archive --format=tar.gz --prefix="$NAME-$VERSION/" -o "$TARBALL" HEAD

rpmbuild -bs \
    --define "_topdir $TOPDIR" \
    "$SPEC"

echo "SRPM written under: $SRPMS"
ls -1 "$SRPMS"/"$NAME"-*.src.rpm
