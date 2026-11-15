import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:meta/meta.dart';

/// `chmod`, which `dart:io` has not got and a file written into a guest cannot
/// do without.
///
/// Through libc rather than `chmod(1)`: iOS refuses to start a process, which
/// is the same refusal that put an interpreter on that platform. The symbol is
/// in the process on both.
///
/// The iOS sandbox refuses set-id bits, and iOS 27 the sticky bit too, failing
/// the whole call: `/tmp` stayed 0755 and apt's `_apt` user could not write its
/// temporary files. So a refused mode is applied without those bits, and they
/// are recorded where `realfs` reads them back.
void chmodGuestFile(String path, int mode) {
  final chmod = _chmod;
  if (chmod == null) return;
  final pointer = path.toNativeUtf8();
  try {
    if (chmod(pointer.cast(), mode) == 0) return;
    final special = mode & _specialBits;
    if (special == 0) return;
    final baseMode = mode & ~_specialBits;
    final rwMode = baseMode | 0x180;
    if (chmod(pointer.cast(), rwMode) != 0) return;
    _recordSpecialBits(pointer, special, rawPath: path);
    if (baseMode != rwMode) {
      chmod(pointer.cast(), baseMode);
    }
  } finally {
    malloc.free(pointer);
  }
}

/// Set-id and sticky: `S_ISUID | S_ISGID | S_ISVTX`.
const _specialBits = 0xE00;

/// Writes `com.flul.meta`, the record `realfs` keeps of what the host refused:
/// uid and gid (preserved if present, else unset), then the special bits, each
/// a little-endian u32. See `crates/fs/realfs/src/meta.rs`. Only `realfs` reads
/// it, so only Apple's platforms write it.
void _recordSpecialBits(Pointer<Utf8> path, int bits, {String? rawPath}) {
  final setxattr = _setxattr;
  if (setxattr == null) return;
  final name = 'com.flul.meta'.toNativeUtf8();
  final value = malloc<Uint32>(3);
  try {
    final words = value.asTypedList(3);
    final getxattr = _getxattr;
    bool readExisting() =>
        getxattr != null &&
        getxattr(path.cast(), name.cast(), value.cast(), 12, 0, 1) == 12;

    if (!readExisting()) {
      words[0] = 0xFFFFFFFF;
      words[1] = 0xFFFFFFFF;
    }
    words[2] = bits;
    // `XATTR_NOFOLLOW`: the entry itself, as `realfs` reads it.
    if (setxattr(path.cast(), name.cast(), value.cast(), 12, 0, 1) == 0) {
      return;
    }
    final chmod = _chmod;
    if (chmod == null) return;
    final pathString = rawPath ?? path.toDartString();
    if (FileSystemEntity.typeSync(pathString, followLinks: false) ==
        FileSystemEntityType.link) {
      return;
    }
    final stat = FileStat.statSync(pathString);
    if (stat.type == FileSystemEntityType.notFound ||
        stat.type == FileSystemEntityType.link) {
      return;
    }
    final origMode = stat.mode & 0xfff;
    final tempMode = origMode | 0x180;
    if (tempMode == origMode || chmod(path.cast(), tempMode) != 0) return;
    try {
      if (!readExisting()) {
        words[0] = 0xFFFFFFFF;
        words[1] = 0xFFFFFFFF;
      }
      words[2] = bits;
      setxattr(path.cast(), name.cast(), value.cast(), 12, 0, 1);
    } finally {
      chmod(path.cast(), origMode);
    }
  } finally {
    malloc.free(name);
    malloc.free(value);
  }
}

@visibleForTesting
void recordSpecialBitsForTest(String path, int bits) {
  final pointer = path.toNativeUtf8();
  try {
    _recordSpecialBits(pointer, bits, rawPath: path);
  } finally {
    malloc.free(pointer);
  }
}

final _getxattr = () {
  if (!Platform.isIOS && !Platform.isMacOS) return null;
  try {
    return DynamicLibrary.process().lookupFunction<
      IntPtr Function(
        Pointer<Char>,
        Pointer<Char>,
        Pointer<Void>,
        Size,
        Uint32,
        Int,
      ),
      int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, int, int, int)
    >('getxattr');
  } catch (_) {
    return null;
  }
}();

final _setxattr = () {
  if (!Platform.isIOS && !Platform.isMacOS) return null;
  try {
    return DynamicLibrary.process().lookupFunction<
      Int Function(
        Pointer<Char>,
        Pointer<Char>,
        Pointer<Void>,
        Size,
        Uint32,
        Int,
      ),
      int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, int, int, int)
    >('setxattr');
  } catch (_) {
    return null;
  }
}();

/// `link`, for the same reason [chmodGuestFile] exists: `dart:io` offers only
/// symbolic links, and a rootfs is full of hard ones.
///
/// Ubuntu 26.04 ships uutils coreutils as one 10 MB multi-call binary with 115
/// hard links pointing at it, and Perl adds another. Turning those into copies
/// would cost a gigabyte; turning them into symlinks is what the tar reader
/// already did by accident, and it produced links pointing nowhere — see the
/// unpacking code. A real hard link is what the archive asked for and costs
/// nothing.
///
/// Returns whether it worked. Failure is worth handling rather than throwing:
/// the caller has a correct fallback, and a system missing `ls` is worse than
/// one whose `ls` is a symlink.
bool linkGuestFile(String target, String path) {
  final link = _link;
  if (link == null) return false;
  final targetPtr = target.toNativeUtf8();
  final pathPtr = path.toNativeUtf8();
  try {
    return link(targetPtr.cast(), pathPtr.cast()) == 0;
  } catch (_) {
    return false;
  } finally {
    malloc.free(targetPtr);
    malloc.free(pathPtr);
  }
}

final _link = () {
  try {
    return DynamicLibrary.process().lookupFunction<
      Int Function(Pointer<Char>, Pointer<Char>),
      int Function(Pointer<Char>, Pointer<Char>)
    >('link');
  } catch (_) {
    return null;
  }
}();

final _chmod = () {
  try {
    final process = DynamicLibrary.process();
    // `mode_t` is `uint16_t` on Darwin and `unsigned int` everywhere else this
    // ships, and the lookup has to name the width the call will actually use.
    if (Platform.isIOS || Platform.isMacOS) {
      return process.lookupFunction<
        Int Function(Pointer<Char>, Uint16),
        int Function(Pointer<Char>, int)
      >('chmod');
    }
    return process.lookupFunction<
      Int Function(Pointer<Char>, UnsignedInt),
      int Function(Pointer<Char>, int)
    >('chmod');
  } catch (_) {
    return null;
  }
}();

/// Writes [content] to [file] and assigns [mode] if [file] did not exist yet,
/// preventing host `umask(0)` (set by `become_first_process`) from leaving new
/// guest files world-writable (`0666`).
Future<void> writeGuestTextFile(
  File file,
  String content, {
  int mode = 0x1A4,
}) async {
  final existed = await file.exists();
  await file.writeAsString(content);
  if (!existed) chmodGuestFile(file.path, mode);
}

/// Repairs guest directory modes on an existing rootfs so `/` is `0755` (not
/// `0777` from host `umask(0)`) and `/tmp` + `/var/tmp` are `01777`.
void repairGuestModes(String root) {
  if (FileSystemEntity.typeSync(root, followLinks: false) ==
      FileSystemEntityType.directory) {
    if ((FileStat.statSync(root).mode & 0xfff) == 0x1ff) {
      chmodGuestFile(root, 0x1ED);
    }
  }
  for (final rel in const ['tmp', 'var/tmp']) {
    final dir =
        '$root${Platform.pathSeparator}${rel.replaceAll('/', Platform.pathSeparator)}';
    if (FileSystemEntity.typeSync(dir, followLinks: false) ==
        FileSystemEntityType.directory) {
      chmodGuestFile(dir, 0x3FF);
    }
  }
}
