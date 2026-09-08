import 'dart:io';
import 'package:flutter/material.dart';
import 'camera/camera_backend.dart';
import 'camera/camera_screen.dart';

void main() => runApp(const CameraApp());

class CameraApp extends StatelessWidget {
  const CameraApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Camera',
    debugShowCheckedModeBanner: false,
    theme: ThemeData(
      brightness: Brightness.dark,
      scaffoldBackgroundColor: const Color(0xFF0B0D0E),
      colorScheme: const ColorScheme.dark(primary: Color(0xFF347BFA)),
      fontFamily: 'Helvetica Neue',
      splashFactory: NoSplash.splashFactory,
    ),
    home: CameraScreen(backend: Platform.isLinux ? CameraBackend() : null),
  );
}
