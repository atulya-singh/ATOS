# Filesystems

Code: `src/kernel/fs/`.

## VFS (`vfs.c`, `vfs.h`)

Every file, directory, device, and socket is a **vnode** with an ops
table:

- `read` and `write`, at a byte offset
- `lookup`, `readdir`, `create`, `mkdir`, `remove`, and `rename`, for
  directories
- `truncate`
- `release`

Vnodes are refcounted. `release` runs when the last reference drops.

- **Mounts.** Mounts attach a root vnode at a path. Lookups resolve
  through the mount with the longest matching prefix, so `/`, `/dev`, and
  `/disk` nest. Paths are normalized lexically first (`lib/path.c`,
  host-tested): `.`, `..`, and repeated slashes are resolved.
- **Open files.** An open file (`struct file`) is a vnode plus an offset
  and access flags. It is refcounted, so it can be shared by `dup2` and
  `fork`.
- **fd tables.** Each task has a table of 16 descriptors.
- **Namespace changes.** `vfs_mkdir`, `vfs_unlink`, `vfs_rmdir`, and
  `vfs_rename` resolve the parent directory and hand the leaf name to its
  ops. Mount points can't be removed or renamed (`-EBUSY`), and a rename
  across filesystems fails with `-EXDEV`.

## initrd (`initrd.c`, `tar.c`)

The root filesystem is a ustar archive that Limine loads as a module
(`initrd.tar`, built from `rootfs/`, `/bin`, and the ports). At boot it
is parsed into an in-memory tree. File data is read straight from the
module, with no copy. The tree is read-only.

`tar.c` parses the octal header fields and verifies checksums. It is
hardware-free and host-tested.

## devfs (`devfs.c`)

Mounted at `/dev`:

- `/dev/console`: reads come from the keyboard in raw mode (block for
  one key, then return whatever else is queued; echo and line editing are
  the reader's job, as in the shell); writes go to serial and the
  framebuffer.
- `/dev/null`: the usual semantics.

## FAT32 (`fat.c`, `fat_names.c`)

The first block device holding FAT32 (a whole-disk volume, no partition
table) is mounted at `/disk`.

- **Names.** VFAT long names are read and written. Without one, the 8.3
  name is used, with its NT lowercase flags applied. Lookups are
  case-insensitive.
- **Writes.** Files can be created (with generated `~N` aliases and
  long-name entries where needed), grown, appended to, and truncated.
  Every change is written through, and FAT copies are mirrored, so
  pulling the plug never leaves the mirrors disagreeing.
- **FSInfo.** Its hints are marked "unknown" rather than maintained, so
  they can never be wrong.
- **Directories.** `mkdir` writes `.` and `..` into a fresh cluster.
  `rmdir` requires the directory to be empty. Removing marks every slot of
  the entry (long-name pieces too) deleted, then frees the cluster chain.
- **Rename.** The new entry is written before the old one is deleted, so
  a crash leaves two names, never none. An existing file at the target is
  replaced; an existing directory is not (`-EEXIST`). A directory that
  changes parent gets its `..` rewritten. Moving a directory into its own
  subtree is caught by walking `..` up from the target (`-EINVAL`).
  Case-only renames (`a.txt` to `A.txt`) work.
- **Open files.** The volume keeps a list of live nodes. Removing an open
  file or directory fails with `-EBUSY` instead of leaving a node on
  freed clusters, and rename re-points the nodes of the entry it moves.
- **Locking.** One mutex per volume serializes access.

`fat_names.c` holds the name codec (8.3 encoding, LFN checksums, alias
generation, date/time encoding). It is hardware-free and host-tested.

The smoke test checks the guest's writes from the host with `mtools`
and `fsck.fat`.
