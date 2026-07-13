#!/bin/sh
# Populate directory tree inside an ext2 image using debugfs batch mode.
set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=$(dirname "$SCRIPT_DIR")

IMG_PATH=${1:-$ROOT_DIR/disk.img}
# Ensure IMG_PATH is absolute
if [ -f "$IMG_PATH" ]; then
    IMG_PATH=$(readlink -f "$IMG_PATH")
else
    # If it doesn't exist, readlink -f might fail or return nothing.
    # Just make it absolute manually if it's relative.
    case "$IMG_PATH" in
        /*) ;;
        *) IMG_PATH="$PWD/$IMG_PATH" ;;
    esac
fi

SRC_DIR=${2:-}
[ -n "$SRC_DIR" ] && SRC_DIR=$(readlink -f "$SRC_DIR")
DST_DIR=${3:-}

if [ -z "$SRC_DIR" ] || [ -z "$DST_DIR" ]; then
  echo "Usage: $0 <img_path> <src_dir> <dst_dir>"
  exit 1
fi

# Remove trailing slashes
SRC_DIR=${SRC_DIR%/}
DST_DIR=${DST_DIR%/}

if [ ! -d "$SRC_DIR" ]; then
  echo "Missing source directory: $SRC_DIR" >&2
  exit 1
fi

CMDS_FILE=$(mktemp)

# Start generating commands
echo "cd /" > "$CMDS_FILE"

ensure_dir_cmds() {
  cur=""
  old_ifs=$IFS
  IFS='/'
  # shellcheck disable=SC2086
  set -- $1
  IFS=$old_ifs

  for comp in "$@"; do
    [ -n "$comp" ] || continue
    if [ -z "$cur" ]; then
      cur="$comp"
    else
      cur="$cur/$comp"
    fi
    echo "mkdir $cur" >> "$CMDS_FILE"
  done
}

ensure_dir_cmds "$DST_DIR"

(
  cd "$SRC_DIR"
  # First create all directories
  find . -type d | while IFS= read -r dir_path; do
    rel_path=${dir_path#./}
    [ "$rel_path" = "." ] && continue
    echo "mkdir $DST_DIR/$rel_path" >> "$CMDS_FILE"
    mode=$(stat -c %a "$dir_path")
    echo "set_inode_field $DST_DIR/$rel_path mode 040$mode" >> "$CMDS_FILE"
  done

  # Then write files and symlinks
  find . -type f -o -type l | while IFS= read -r file_path; do
    rel_path=${file_path#./}
    if [ -L "$file_path" ]; then
      target=$(readlink "$file_path")
      echo "symlink $DST_DIR/$rel_path $target" >> "$CMDS_FILE"
    else
      echo "write $SRC_DIR/$rel_path $DST_DIR/$rel_path" >> "$CMDS_FILE"
      mode=$(stat -c %a "$file_path")
      echo "set_inode_field $DST_DIR/$rel_path mode 0100$mode" >> "$CMDS_FILE"
      mtime=$(stat -c %Y "$file_path")
      echo "set_inode_field $DST_DIR/$rel_path mtime @$mtime" >> "$CMDS_FILE"
    fi
  done

  # Creating children changes directory mtimes. Restore them only after the
  # complete tree has been populated so metadata-based caches (notably
  # Fontconfig) remain valid in the image.
  find . -depth -type d | while IFS= read -r dir_path; do
    rel_path=${dir_path#./}
    [ "$rel_path" = "." ] && continue
    mtime=$(stat -c %Y "$dir_path")
    echo "set_inode_field $DST_DIR/$rel_path mtime @$mtime" >> "$CMDS_FILE"
  done
)

# Flatpak/container development shells may expose debugfs only through the
# host filesystem. Prefer that binary when available so a successful setup
# cannot silently leave the disk image unchanged.
DEBUGFS_BIN=debugfs
if [ -x /run/host/usr/bin/debugfs ]; then
  DEBUGFS_BIN=/run/host/usr/bin/debugfs
fi

# Execute commands in a single debugfs call.
# Existing-directory diagnostics are expected during incremental population,
# so suppress batch output while still treating a debugfs process failure as fatal.
if ! "$DEBUGFS_BIN" -w -f "$CMDS_FILE" "$IMG_PATH" > /dev/null 2>&1; then
  echo "Failed to populate ext2 image with $DEBUGFS_BIN" >&2
  rm -f "$CMDS_FILE"
  exit 1
fi

rm -f "$CMDS_FILE"
