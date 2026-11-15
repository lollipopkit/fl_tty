import 'dart:ffi';
import 'dart:io';

import 'package:archive/archive.dart';
import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:fl_tty/fl_tty.dart';

/// What mode a directory lands with, which is two requirements at once.
///
/// `realfs` hands the host's mode straight to the guest, and the host process
/// is this app rather than root — so uid 0 in the guest buys nothing, and a
/// directory the archive ships at 0555 is one a package manager cannot create
/// its temp files in. Rocky ships 17 of those. Owner rwx therefore has to be
/// forced on whatever the archive says.
///
/// The other requirement is everything else the mode carries. `/tmp` is 1777
/// in every distribution, and a rootfs whose directory modes are all 0755 is
/// one `rpm --verify` and `dpkg --verify` report as altered — the same
/// complaint stripping locales would cause, and the reason `shellbox-rootfs`
/// does not strip them.
void main() {
  late Directory dir;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('sbm-modes-');
  });

  tearDown(() async {
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  const source = RootfsSource(
    url: 'https://example.test/rootfs.tar.gz',
    sha256: '0000000000000000000000000000000000000000000000000000000000000000',
    sizeBytes: 1,
    layout: LinuxRootfsLayout.plain,
    compression: LinuxRootfsCompression.gzip,
    followsMirror: false,
  );

  /// Unpacks a plain rootfs of directories at the modes given.
  Future<Directory> unpack(Map<String, int> directories) async {
    final archive = Archive();
    for (final entry in directories.entries) {
      archive.add(ArchiveFile.directory(entry.key)..mode = entry.value);
    }
    // A file inside one of them, so the tree is not only directories.
    archive.add(ArchiveFile.string('etc/hostname', 'box')..mode = 0x1a4);

    final file = File('${dir.path}/rootfs.tar.gz');
    await file.writeAsBytes(
      GZipEncoder().encodeBytes(TarEncoder().encodeBytes(archive)),
    );

    final into = Directory('${dir.path}/root')..createSync(recursive: true);
    await RootfsUnpack.extract(file, into, source: source);
    return into;
  }

  int modeOf(Directory into, String path) =>
      Directory('${into.path}/$path').statSync().mode & 0xfff;

  test('a directory the archive ships read-only becomes writable', () {
    // The failure this whole rule exists for: `rpm` reports it as
    // `cpio: open failed`, which reads as a broken download.
    return unpack({'usr': 0x1ed, 'usr/bin': 0x16d}).then((into) {
      expect(modeOf(into, 'usr/bin') & 0x1c0, 0x1c0, reason: 'owner rwx');
      // And nothing else was added: 0555 becomes 0755, not 0777.
      expect(modeOf(into, 'usr/bin'), 0x1ed);
    });
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test('and keeps the bits that were not in the way', () async {
    // 1777 for /tmp, 2755 for a setgid directory. Neither stops this app
    // writing there, so neither is something to drop — and both are what a
    // package manager's --verify compares against.
    final into = await unpack({
      'tmp': 0x3ff, // 1777
      'var': 0x1ed,
      'var/log': 0x5ed, // 2755
    });

    expect(modeOf(into, 'tmp') & 0x200, 0x200, reason: 'sticky');
    expect(modeOf(into, 'tmp') & 0x1ff, 0x1ff, reason: '777');
    expect(modeOf(into, 'var/log') & 0x400, 0x400, reason: 'setgid');
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test('a /tmp the archive ships at 0755 is 1777 once unpacked', () async {
    // shellbox-rootfs repacked without `tar -p`, which drops the sticky bit
    // and applies the builder's umask.
    final into = await unpack({'tmp': 0x1ed, 'var': 0x1ed, 'var/tmp': 0x1ed});

    expect(modeOf(into, 'tmp'), 0x3ff);
    expect(modeOf(into, 'var/tmp'), 0x3ff);
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test('an ordinary directory is left exactly as the archive had it', () async {
    final into = await unpack({'etc': 0x1ed});

    expect(modeOf(into, 'etc'), 0x1ed);
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test('a directory only named as a parent is still usable', () async {
    // `usr/lib/x` with no entry of its own for `usr/lib`. Even under host
    // `umask(0)`, implicit parent directories are pinned to `0755`.
    final into = await unpack({'usr/lib/x': 0x1ed});

    expect(modeOf(into, 'usr/lib'), 0x1ed);
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test(
    'root directory becomes 0755 even when pre-created 0777 or archived 0555',
    () async {
      Future<File> writeArchive(Map<String, int> dirs, String name) async {
        final archive = Archive();
        for (final entry in dirs.entries) {
          archive.add(ArchiveFile.directory(entry.key)..mode = entry.value);
        }
        archive.add(ArchiveFile.string('etc/hostname', 'box')..mode = 0x1a4);
        final file = File('${dir.path}/$name');
        await file.writeAsBytes(
          GZipEncoder().encodeBytes(TarEncoder().encodeBytes(archive)),
        );
        return file;
      }

      final noRootTar = await writeArchive({'etc': 0x1ed}, 'no_root.tar.gz');
      final intoNoRoot = Directory('${dir.path}/root_no_entry')
        ..createSync(recursive: true);
      chmodGuestFile(intoNoRoot.path, 0x1ff); // simulate Directory.create under umask(0)
      await RootfsUnpack.extract(noRootTar, intoNoRoot, source: source);
      expect(Directory(intoNoRoot.path).statSync().mode & 0xfff, 0x1ed);

      final roRootTar = await writeArchive({
        './': 0x16d, // 0555
        'etc': 0x1ed,
      }, 'ro_root.tar.gz');
      final intoRoRoot = Directory('${dir.path}/root_ro_entry')
        ..createSync(recursive: true);
      chmodGuestFile(intoRoRoot.path, 0x1ff);
      await RootfsUnpack.extract(roRootTar, intoRoRoot, source: source);
      expect(Directory(intoRoRoot.path).statSync().mode & 0xfff, 0x1ed);
    },
    skip: Platform.isWindows ? 'requires POSIX file modes' : null,
  );

  test('repairGuestModes fixes 0777 root and 0755 tmp directories', () async {
    final root = Directory('${dir.path}/repair')..createSync(recursive: true);
    final tmp = Directory('${root.path}/tmp')..createSync(recursive: true);
    final varTmp = Directory('${root.path}/var/tmp')
      ..createSync(recursive: true);
    chmodGuestFile(root.path, 0x1ff); // 0777
    chmodGuestFile(tmp.path, 0x1ed); // 0755
    chmodGuestFile(varTmp.path, 0x1ed); // 0755

    repairGuestModes(root.path);

    expect(root.statSync().mode & 0xfff, 0x1ed);
    expect(tmp.statSync().mode & 0xfff, 0x3ff);
    expect(varTmp.statSync().mode & 0xfff, 0x3ff);
  }, skip: Platform.isWindows ? 'requires POSIX file modes' : null);

  test(
    'recording special bits preserves existing uid/gid in com.flul.meta',
    () async {
      final target = Directory('${dir.path}/meta-tmp')..createSync();
      final process = DynamicLibrary.process();
      final setxattr = process.lookupFunction<
        Int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, Size, Uint32, Int),
        int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, int, int, int)
      >('setxattr');
      final getxattr = process.lookupFunction<
        IntPtr Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, Size, Uint32, Int),
        int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, int, int, int)
      >('getxattr');

      final pathPtr = target.path.toNativeUtf8();
      final namePtr = 'com.flul.meta'.toNativeUtf8();
      final buf = malloc<Uint32>(3);
      try {
        final words = buf.asTypedList(3);
        words[0] = 42;
        words[1] = 84;
        words[2] = 0;
        expect(
          setxattr(pathPtr.cast(), namePtr.cast(), buf.cast(), 12, 0, 1),
          0,
        );

        recordSpecialBitsForTest(target.path, 0x200);

        words.fillRange(0, 3, 0);
        expect(
          getxattr(pathPtr.cast(), namePtr.cast(), buf.cast(), 12, 0, 1),
          12,
        );
        expect(words[0], 42);
        expect(words[1], 84);
        expect(words[2], 0x200);
      } finally {
        malloc.free(pathPtr);
        malloc.free(namePtr);
        malloc.free(buf);
      }
    },
    skip: !(Platform.isMacOS || Platform.isIOS)
        ? 'requires Darwin xattr API'
        : null,
  );

  test(
    'chmodGuestFile applies 0000 mode and records special bits on read-only 0555 files',
    () async {
      final zeroFile = File('${dir.path}/zero')..writeAsStringSync('x');
      chmodGuestFile(zeroFile.path, 0);
      expect(zeroFile.statSync().mode & 0xfff, 0);
      chmodGuestFile(zeroFile.path, 0x1a4);

      if (Platform.isMacOS || Platform.isIOS) {
        final roFile = File('${dir.path}/suid-ro')..writeAsStringSync('bin');
        chmodGuestFile(roFile.path, 0x16d); // 0555
        recordSpecialBitsForTest(roFile.path, 0x800); // 04000

        final process = DynamicLibrary.process();
        final getxattr = process.lookupFunction<
          IntPtr Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, Size, Uint32, Int),
          int Function(Pointer<Char>, Pointer<Char>, Pointer<Void>, int, int, int)
        >('getxattr');
        final pathPtr = roFile.path.toNativeUtf8();
        final namePtr = 'com.flul.meta'.toNativeUtf8();
        final buf = malloc<Uint32>(3);
        try {
          expect(
            getxattr(pathPtr.cast(), namePtr.cast(), buf.cast(), 12, 0, 1),
            12,
          );
          expect(buf.asTypedList(3)[2], 0x800);
          expect(roFile.statSync().mode & 0xfff, 0x16d);
        } finally {
          malloc.free(pathPtr);
          malloc.free(namePtr);
          malloc.free(buf);
        }
      }
    },
    skip: Platform.isWindows ? 'requires POSIX file modes' : null,
  );

  test(
    'recordSpecialBitsForTest does not follow symlinks into chmod',
    () async {
      final target = File('${dir.path}/target-ro')..writeAsStringSync('keep');
      chmodGuestFile(target.path, 0x124); // 0444
      final link = Link('${dir.path}/sym-link')..createSync(target.path);

      recordSpecialBitsForTest(link.path, 0x800);

      expect(target.statSync().mode & 0xfff, 0x124);
    },
    skip: Platform.isWindows ? 'requires POSIX file modes' : null,
  );
}
