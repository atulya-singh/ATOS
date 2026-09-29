#!/usr/bin/env bash
# Builds one port: tools/port.sh ports/<name> build/ports/<name>
#
# A port is a directory with a PORTBUILD recipe (shell variables, in the
# spirit of Arch's PKGBUILD) plus its sources. The result is a staging
# root, build/ports/<name>/root/, that the Makefile merges into the
# initrd, and a line for the package index. See docs/ports.md.
#
# The Makefile passes the userspace toolchain in the environment:
# CC, LD, UCFLAGS, ULDFLAGS, LIBC_OBJS.
set -euo pipefail

src=$1
out=$2
name=$(basename "$src")
die() { echo "port $name: $*" >&2; exit 1; }

PORT_VERSION=
PORT_DESCRIPTION=
PORT_LICENSE=
PORT_PROGRAMS=
PORT_FILES=
PORT_CFLAGS=
# shellcheck source=/dev/null
. "$src/PORTBUILD"
[ -n "$PORT_VERSION" ] || die "PORT_VERSION is not set"
[ -n "$PORT_DESCRIPTION" ] || die "PORT_DESCRIPTION is not set"
[ -n "$PORT_PROGRAMS$PORT_FILES" ] || die "installs nothing (no PORT_PROGRAMS or PORT_FILES)"

rm -rf "$out"
mkdir -p "$out/obj" "$out/root/usr/bin" "$out/root/usr/share/ports"
installed=()

# Each program is built from <prog>_SRCS (default: <prog>.c) and linked
# statically against libc into /usr/bin.
for prog in $PORT_PROGRAMS; do
    var="${prog//-/_}_SRCS"
    srcs=${!var:-$prog.c}
    objs=()
    for s in $srcs; do
        [ -f "$src/$s" ] || die "missing source $s"
        o="$out/obj/$prog/${s%.*}.o"
        mkdir -p "$(dirname "$o")"
        # shellcheck disable=SC2086
        $CC $UCFLAGS $PORT_CFLAGS -I "$src" -c "$src/$s" -o "$o"
        objs+=("$o")
    done
    # shellcheck disable=SC2086
    $LD $ULDFLAGS "${objs[@]}" $LIBC_OBJS -o "$out/root/usr/bin/$prog"
    installed+=("/usr/bin/$prog")
done

# Data files, as <file in the port>:<absolute install path>.
for f in $PORT_FILES; do
    from=${f%%:*}
    to=${f#*:}
    [ -f "$src/$from" ] || die "missing file $from"
    case "$to" in /*) ;; *) die "install path $to is not absolute" ;; esac
    mkdir -p "$out/root$(dirname "$to")"
    cp "$src/$from" "$out/root$to"
    installed+=("$to")
done

# Metadata for pkg(1): one file per port, plus a line for the index.
{
    echo "name: $name"
    echo "version: $PORT_VERSION"
    echo "description: $PORT_DESCRIPTION"
    [ -z "$PORT_LICENSE" ] || echo "license: $PORT_LICENSE"
    echo "files:"
    printf '  %s\n' "${installed[@]}"
} > "$out/root/usr/share/ports/$name"
printf '%s %s %s\n' "$name" "$PORT_VERSION" "$PORT_DESCRIPTION" > "$out/index-line"
touch "$out/.built"
echo "port $name $PORT_VERSION: ${installed[*]}"
