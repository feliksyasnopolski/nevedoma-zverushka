import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'camera_backend.dart';

enum CameraMode { photo, video }

enum PhotoFormat { jpeg, dng }

enum ManualControl { af, ev, iso, exposure, focus, locks }

/// In-memory prototype state. Values describe UI intent, never camera telemetry.
class CameraUiState extends ChangeNotifier {
  CameraUiState({CameraBackend? backend}) : _backend = backend {
    _backend?.onStatus = _applyBackendStatus;
    _backend?.onConnectionChanged = (connected) {
      backendConnected = connected;
      notifyListeners();
    };
    _backend?.onError = (message) {
      errorMessage = message;
      notifyListeners();
    };
    _backend?.connect();
  }

  final CameraBackend? _backend;
  CameraMode mode = CameraMode.photo;
  PhotoFormat format = PhotoFormat.jpeg;
  ManualControl? selectedControl;
  Offset? focusPoint;
  bool controlsExpanded = true;
  bool recording = false;
  bool locked = false;
  bool autofocus = true;
  double ev = 0;
  int isoIndex = 0;
  int exposureIndex = 0;
  double? manualFocus;
  int recentShot = 0;
  PhotoFormat? recentFormat;
  bool backendConnected = false;
  String? errorMessage;
  String backendCameraState = 'unknown';
  String recordingState = 'idle';

  static const isoValues = ['Auto', '100', '200', '400', '800', '1600'];
  static const exposureValues = [
    'Auto',
    '1/30',
    '1/60',
    '1/100',
    '1/250',
    '1/500',
  ];

  void selectMode(CameraMode value) {
    if (recording || value == mode) return;
    if (_backend == null) {
      mode = value;
      notifyListeners();
      return;
    }
    _run(() async {
      await _backend.setMode(value.name);
    });
  }

  void selectFormat(PhotoFormat value) {
    if (mode != CameraMode.photo) return;
    format = value;
    notifyListeners();
  }

  void selectControl(ManualControl value) {
    selectedControl = selectedControl == value ? null : value;
    notifyListeners();
  }

  void toggleControls() {
    controlsExpanded = !controlsExpanded;
    selectedControl = null;
    notifyListeners();
  }

  /// Focus seam: normalized visible-preview coordinates, top-left origin.
  /// This stub only moves the reticle. Future backend mapping belongs here,
  /// including any crop/rotation transform and camera focus capability checks.
  void requestFocus(double x, double y) {
    focusPoint = Offset(x.clamp(0, 1), y.clamp(0, 1));
    notifyListeners();
  }

  void capture() {
    if (mode != CameraMode.photo || recording) return;
    if (_backend != null) {
      final format = this.format.name;
      _run(() async {
        final response = await _backend.capture(format);
        final source = format == 'jpeg'
            ? response['jpeg_path'] as String?
            : response['dng_path'] as String?;
        if (source == null || source.isEmpty) {
          throw CameraBackendException('capture returned no artifact path');
        }
        final saved = await preserveArtifact(
          source,
          format == 'jpeg' ? 'jpg' : 'dng',
        );
        recentShot++;
        recentFormat = this.format;
        errorMessage = 'Saved ${saved.split('/').last}';
        _logOperation('capture $format succeeded: $saved');
        notifyListeners();
      });
      return;
    }
    recentShot++;
    recentFormat = format;
    notifyListeners();
  }

  void toggleRecording() {
    if (_backend == null) {
      mode = CameraMode.video;
      recording = !recording;
      notifyListeners();
      return;
    }
    if (recording) {
      _run(() async {
        final response = await _backend.stopVideo();
        recordingState = response['state'] as String? ?? 'stopping';
        recording = false;
        final source = response['path'] as String?;
        if (source != null && source.isNotEmpty) {
          final saved = await preserveArtifact(
            source,
            'mp4',
            directoryName: 'Videos',
          );
          errorMessage = 'Saved ${saved.split('/').last}';
          _logOperation('video completed and preserved: $saved');
        }
        notifyListeners();
      });
    } else {
      _run(() async {
        final response = await _backend.startVideo();
        recordingState = response['state'] as String? ?? 'starting';
        recording = true;
        notifyListeners();
      });
    }
  }

  Future<void> _run(Future<void> Function() operation) async {
    errorMessage = null;
    notifyListeners();
    try {
      await operation();
    } on CameraBackendException catch (error) {
      _logOperation('operation failed: ${error.message}');
      errorMessage = error.message;
      notifyListeners();
    } on Object catch (error) {
      _logOperation('operation failed: $error');
      errorMessage = '$error';
      notifyListeners();
    }
  }

  void _logOperation(String message) {
    // The backend owns detailed transport/file diagnostics; this identifies
    // which UI operation produced them in the tablet launch terminal.
    stderr.writeln('[camera-app] $message');
  }

  void _applyBackendStatus(Map<String, dynamic> status) {
    if (status['mode'] == 'photo') mode = CameraMode.photo;
    if (status['mode'] == 'video') mode = CameraMode.video;
    backendCameraState = status['state'] as String? ?? 'unknown';
    final error = status['last_error'];
    if (error is Map<String, dynamic>) errorMessage = error['message'] as String?;
    notifyListeners();
  }

  @override
  void dispose() {
    unawaited(_backend?.dispose());
    super.dispose();
  }

  void updateManual(VoidCallback update) {
    update();
    notifyListeners();
  }
}
