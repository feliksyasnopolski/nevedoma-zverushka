import 'dart:io';

import 'package:camera/camera/camera_screen.dart';
import 'package:camera/camera/camera_state.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:integration_test/integration_test.dart';

import 'package:camera/main.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('Linux backend capture and recording flow', (tester) async {
    await tester.pumpWidget(const CameraApp());
    final state = tester.widget<ManualRail>(find.byType(ManualRail)).state;
    for (var i = 0; i < 30 && !state.backendConnected; i++) {
      await tester.pump(const Duration(milliseconds: 200));
    }
    expect(state.backendConnected, isTrue);

    await tester.tap(find.text('DNG'));
    await tester.pump();
    await tester.tap(find.byKey(const Key('shutter')));
    await _waitFor(tester, () => state.recentShot == 1);
    expect(_newArtifacts('dng'), isNotEmpty);

    await tester.tap(find.text('JPEG'));
    await tester.pump();
    await tester.tap(find.byKey(const Key('shutter')));
    await _waitFor(tester, () => state.recentShot == 2);
    expect(_newArtifacts('jpg'), isNotEmpty);

    await tester.tap(find.text('Video'));
    await _waitFor(tester, () => state.mode == CameraMode.video);
    await tester.tap(find.byKey(const Key('record')));
    await _waitFor(tester, () => state.recording);
    await tester.pump(const Duration(seconds: 3));
    await tester.tap(find.byKey(const Key('record')));
    await _waitFor(tester, () => !state.recording);
    expect(_newArtifacts('mp4', directoryName: 'Videos'), isNotEmpty);
  });
}

Future<void> _waitFor(WidgetTester tester, bool Function() condition) async {
  for (var i = 0; i < 100 && !condition(); i++) {
    await tester.pump(const Duration(milliseconds: 200));
  }
  expect(condition(), isTrue);
}

List<File> _newArtifacts(String extension, {String directoryName = 'Pictures'}) => Directory(
        '${Platform.environment['HOME'] ?? Directory.current.path}/$directoryName')
    .listSync()
    .whereType<File>()
    .where((file) => file.path.endsWith('.$extension') &&
        file.uri.pathSegments.last.startsWith('RCAM_'))
    .toList();
