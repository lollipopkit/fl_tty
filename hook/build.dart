import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

/// Builds fl_tty's native library, and on Android arm64 the proot pair.
///
/// One library, `fl_tty`, everywhere: the pty (`src/flutter_pty.c`, which
/// `#include`s the rest of it). On iOS arm64 the iul shim is compiled into the
/// same library and linked against the engine's static libraries, which
/// `scripts/ensure-iul-libs.sh` fetches for the submodule's revision (or
/// builds, without a release for it).
///
/// User-defines, in the app's pubspec under `hooks: user_defines: fl_tty:`:
///
///   iul: false        an iOS build without the engine: `IosRootfs.isAvailable`
///                     answers false and nothing of iul is linked.
///   proot: false      an Android build without proot.
///
/// proot built ahead of time by running `scripts/build-proot-android.sh` (a
/// build that must not reach the network) is in `build/proot-android/out` and
/// is used as it is.
///
/// proot needs the app to set `packaging.jniLibs.useLegacyPackaging = true`:
/// otherwise nothing is extracted from the APK and there is nowhere to
/// `execve` it from.
void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;
    final code = input.config.code;
    final root = input.packageRoot;

    final iul = code.targetOS == OS.iOS &&
        code.targetArchitecture == Architecture.arm64 &&
        input.userDefines['iul'] != false;
    final iulSrc = root.resolve('third_party/iul/');
    Uri? iulLibs;
    if (iul) {
      final sim = code.iOS.targetSdk == IOSSdk.iPhoneSimulator;
      iulLibs = root.resolve('build/iul/build-${sim ? 'iossim' : 'ios'}-arm64/');
      await _run('scripts/ensure-iul-libs.sh', [], root, {
        'SBM_IUL': '1',
        'PLATFORM_NAME': sim ? 'iphonesimulator' : 'iphoneos',
        'IUL_BUILD_DIR': iulLibs.toFilePath(),
      });
      for (final lib in _iulLibs) {
        output.dependencies.add(iulLibs.resolve('lib$lib.a'));
      }
      final iulHeader = File.fromUri(iulSrc.resolve('iul.h'));
      if (iulHeader.existsSync()) {
        output.dependencies.add(iulHeader.uri);
      }
    }

    await CBuilder.library(
      name: 'fl_tty',
      assetName: 'fl_tty',
      sources: ['src/flutter_pty.c', if (code.targetOS == OS.iOS) 'iul/sbm_iul.c'],
      includes: [
        'src',
        'iul',
        // A fetched release places iul.h directly in IUL_BUILD_DIR; a local
        // submodule checkout also provides iul.h under third_party/iul/.
        if (iul)
          for (final dir in [iulLibs!, iulSrc])
            if (Directory.fromUri(dir).existsSync()) dir.toFilePath(),
      ],
      std: iul ? 'gnu11' : null,
      defines: {
        // What dart_api_dl.c needs to link as part of this library.
        'DART_SHARED_LIB': null,
        if (code.targetOS == OS.iOS) 'SBM_IUL_ENABLED': iul ? '1' : '0',
      },
      libraries: [if (iul) ..._iulLibs, if (iul) 'sqlite3'],
      libraryDirectories: [if (iul) iulLibs!.toFilePath()],
      flags: [
        // Android 15 ships a 16 KB page size, and a library linked for 4 KB
        // will not load on it.
        if (code.targetOS == OS.android) '-Wl,-z,max-page-size=16384',
      ],
    ).run(input: input, output: output);

    if (code.targetOS == OS.android &&
        code.targetArchitecture == Architecture.arm64 &&
        input.userDefines['proot'] != false) {
      final dir = await _proot(input);
      for (final name in const ['proot', 'proot-loader']) {
        final file = dir.resolve('lib$name.so');
        output.assets.code.add(
          CodeAsset(package: input.packageName, name: name, linkMode: DynamicLoadingBundled(), file: file),
        );
        output.dependencies.add(file);
      }
    }
  });
}

const _iulLibs = ['iul'];

/// Where `libproot.so` and `libproot-loader.so` are: built ahead of time, or
/// built once into the shared output directory with the NDK this build uses.
Future<Uri> _proot(BuildInput input) async {
  bool built(Uri dir) =>
      File.fromUri(dir.resolve('libproot.so')).existsSync() &&
      File.fromUri(dir.resolve('libproot-loader.so')).existsSync();
  final ahead = input.packageRoot.resolve('build/proot-android/out/');
  if (built(ahead)) return ahead;
  final out = input.outputDirectoryShared.resolve('proot/');
  if (built(out)) return out;
  // .../ndk/<version>/toolchains/llvm/prebuilt/<host>/bin/clang
  final cc = input.config.code.cCompiler?.compiler;
  if (cc == null) throw StateError('No NDK compiler to build proot with');
  final ndk = cc.resolve('../../../../../');
  await _run('scripts/build-proot-android.sh', [], input.packageRoot, {
    'ANDROID_NDK_HOME': ndk.toFilePath(),
    'PROOT_OUT_DIR': out.toFilePath(),
    'PROOT_BUILD_DIR': input.outputDirectoryShared.resolve('proot-build/').toFilePath(),
  });
  return out;
}

Future<void> _run(String script, List<String> args, Uri root, Map<String, String> env) async {
  final r = await Process.run(
    'bash',
    [root.resolve(script).toFilePath(), ...args],
    workingDirectory: root.toFilePath(),
    environment: env,
  );
  if (r.exitCode != 0) {
    throw ProcessException(script, args, '${r.stdout}\n${r.stderr}', r.exitCode);
  }
}
