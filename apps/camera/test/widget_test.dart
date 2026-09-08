import 'package:camera/main.dart';
import 'package:camera/camera/camera_state.dart';
import 'package:camera/camera/camera_screen.dart';
import 'package:camera/camera/camera_preview.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('Camera shell interactions and recording guard', (tester) async {
    tester.view.physicalSize = const Size(1180, 780);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await exerciseCamera(tester);
  });
  testWidgets('Minimum macOS window fits without layout errors', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 612);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(const CameraApp());
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await tester.tap(find.byKey(const Key('control-focus')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
}

/// Shared acceptance flow, also executed inside the native macOS app.
Future<void> exerciseCamera(WidgetTester tester) async {
  await tester.pumpWidget(const CameraApp());
  await tester.pumpAndSettle();
  final state = tester.widget<ManualRail>(find.byType(ManualRail)).state;
  expect(find.text('12 MP  ·  JPEG'), findsOneWidget);
  await tester.tap(find.text('DNG'));
  await tester.pumpAndSettle();
  expect(state.format, PhotoFormat.dng);
  expect(find.text('12 MP  ·  DNG'), findsOneWidget);
  await tester.tap(find.byKey(const Key('shutter')));
  await tester.pumpAndSettle();
  expect(find.text('MOCK 1'), findsOneWidget);
  expect(state.recentFormat, PhotoFormat.dng);
  await tester.tap(find.text('JPEG'));
  await tester.pumpAndSettle();
  expect(state.format, PhotoFormat.jpeg);
  final rect = tester.getRect(find.byKey(const Key('preview-touch')));
  await tester.tapAt(rect.topLeft + Offset(rect.width * .25, rect.height * .4));
  await tester.pumpAndSettle();
  expect(state.focusPoint!.dx, closeTo(.25, .001));
  expect(state.focusPoint!.dy, closeTo(.4, .001));
  await tester.tapAt(rect.topLeft + Offset(rect.width * .7, rect.height * .6));
  await tester.pumpAndSettle();
  expect(
    tester.widget<CameraPreview>(find.byType(CameraPreview)).focusPoint!.dx,
    closeTo(.7, .001),
  );
  for (final control in ManualControl.values) {
    await tester.ensureVisible(find.byKey(Key('control-${control.name}')));
    await tester.tap(find.byKey(Key('control-${control.name}')));
    await tester.pumpAndSettle();
    expect(state.selectedControl, control);
    expect(find.byKey(const Key('manual-panel')), findsOneWidget);
    expect(tester.takeException(), isNull);
  }
  await tester.tap(find.text('Locked'));
  await tester.pumpAndSettle();
  expect(state.locked, isTrue);
  await tester.tap(find.byTooltip('Close control'));
  await tester.pumpAndSettle();
  await tester.ensureVisible(find.byKey(const Key('toggle-controls')));
  await tester.tap(find.byKey(const Key('toggle-controls')));
  await tester.pumpAndSettle();
  expect(find.byKey(const Key('control-af')), findsNothing);
  await tester.tap(find.byKey(const Key('toggle-controls')));
  await tester.pumpAndSettle();
  await tester.tap(find.text('Video'));
  await tester.pumpAndSettle();
  expect(state.mode, CameraMode.video);
  expect(find.text('DNG'), findsNothing);
  await tester.tap(find.byKey(const Key('record')));
  await tester.pumpAndSettle();
  expect(state.recording, isTrue);
  expect(find.text('REC  ·  SIMULATED'), findsOneWidget);
  await tester.tap(find.text('Photo'));
  await tester.pumpAndSettle();
  expect(state.mode, CameraMode.video);
  await tester.tap(find.byKey(const Key('record')));
  await tester.pumpAndSettle();
  expect(state.recording, isFalse);
  await tester.tap(find.text('Photo'));
  await tester.pumpAndSettle();
  expect(state.mode, CameraMode.photo);
  expect(find.text('12 MP  ·  JPEG'), findsOneWidget);
  expect(tester.takeException(), isNull);
}
