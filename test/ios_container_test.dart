import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:fl_tty/src/ish/ios_rootfs.dart';

/// The systems moving into the App Group container the File Provider serves.
void main() {
  late Directory tmp;
  late String from;
  late String to;

  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('fl_tty_container_');
    from = '${tmp.path}/Library/Application Support/linux';
    to = '${tmp.path}/group/File Provider Storage/linux';
  });

  tearDown(() => tmp.delete(recursive: true));

  Future<void> system(String container, String id) async {
    await Directory('$container/$id/etc').create(recursive: true);
    await File('$container/$id/.installed').writeAsString('alpine\n3.22\nMine\n');
  }

  test('the systems move whole', () async {
    await system(from, 'alpine');

    expect(await IosRootfs.adoptContainer(from: from, to: to), to);

    expect(await File('$to/alpine/.installed').exists(), true);
    expect(await Directory(from).exists(), false);
  });

  test('nothing to move answers the new place', () async {
    expect(await IosRootfs.adoptContainer(from: from, to: to), to);
    expect(await Directory(to).exists(), false);
  });

  test('an empty new place is replaced', () async {
    await system(from, 'alpine');
    await Directory(to).create(recursive: true);

    expect(await IosRootfs.adoptContainer(from: from, to: to), to);

    expect(await File('$to/alpine/.installed').exists(), true);
  });

  test('systems on both sides end up together', () async {
    await system(from, 'old');
    await system(to, 'new');

    expect(await IosRootfs.adoptContainer(from: from, to: to), to);

    expect(await File('$to/new/.installed').exists(), true);
    expect(await File('$to/old/.installed').exists(), true);
    expect(await Directory(from).exists(), false);
  });

  test('an id taken in the new place is not replaced', () async {
    await system(from, 'alpine');
    await File('$from/alpine/etc/mine').writeAsString('old');
    await system(to, 'alpine');

    expect(await IosRootfs.adoptContainer(from: from, to: to), to);

    expect(await File('$to/alpine/etc/mine').exists(), false);
    expect(await File('$from/alpine/etc/mine').readAsString(), 'old');
  });

  test('a move that fails keeps the old place', () async {
    await system(from, 'alpine');
    // A file where the new place's parent has to be.
    await File('${tmp.path}/group').writeAsString('');

    expect(await IosRootfs.adoptContainer(from: from, to: to), from);

    expect(await File('$from/alpine/.installed').exists(), true);
  });
}
