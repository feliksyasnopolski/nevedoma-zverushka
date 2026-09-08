import 'dart:async';
import 'dart:convert';
import 'dart:io';

typedef BackendStatusCallback = void Function(Map<String, dynamic> status);

void _log(String message) {
  stderr.writeln('[camera-app] $message');
}

class CameraBackend {
  CameraBackend({this.onStatus, this.onConnectionChanged, this.onError,
      this.baseUri = 'http://127.0.0.1:3909'});
  BackendStatusCallback? onStatus;
  void Function(bool connected)? onConnectionChanged;
  void Function(String message)? onError;
  final String baseUri;
  WebSocket? _socket;
  Timer? _reconnect;
  bool _closed = false;

  void connect() {
    _closed = false;
    _connectEvents();
  }

  Future<Map<String, dynamic>> setMode(String mode) =>
      _request('PUT', '/v1/mode', body: {'mode': mode});
  Future<Map<String, dynamic>> capture(String format) =>
      _request('POST', '/v1/capture?format=$format');
  Future<Map<String, dynamic>> startVideo() => _request('POST', '/video/start');
  Future<Map<String, dynamic>> stopVideo() => _request('POST', '/video/stop');

  Future<void> dispose() async {
    _closed = true;
    _reconnect?.cancel();
    final socket = _socket;
    _socket = null;
    await socket?.close();
  }

  Future<Map<String, dynamic>> _request(String method, String path,
      {Map<String, dynamic>? body}) async {
    final client = HttpClient();
    try {
      _log('HTTP $method $path');
      final request = await client.openUrl(method, Uri.parse('$baseUri$path'));
      request.headers.contentType = ContentType.json;
      if (body != null) request.write(jsonEncode(body));
      final response = await request.close();
      final text = await response.transform(utf8.decoder).join();
      final decoded = text.isEmpty
          ? <String, dynamic>{}
          : jsonDecode(text) as Map<String, dynamic>;
      _log('HTTP $method $path -> ${response.statusCode}');
      if (response.statusCode < 200 || response.statusCode >= 300) {
        final error = decoded['error'];
        final message = error is Map<String, dynamic>
            ? '${error['message'] ?? 'backend request failed'}'
            : 'backend request failed (${response.statusCode})';
        throw CameraBackendException(message);
      }
      return decoded;
    } on CameraBackendException {
      rethrow;
    } on Object catch (error) {
      _log('HTTP $method $path failed: $error');
      throw CameraBackendException('$error');
    } finally {
      client.close(force: true);
    }
  }

  Future<void> _connectEvents() async {
    if (_closed) return;
    try {
      final httpUri = Uri.parse(baseUri);
      final eventsUri = Uri(
        scheme: httpUri.scheme == 'https' ? 'wss' : 'ws',
        host: httpUri.host,
        port: httpUri.port,
        path: '/v1/events',
      );
      _log('WebSocket connecting: $eventsUri');
      final socket = await WebSocket.connect(eventsUri.toString());
      if (_closed) {
        await socket.close();
        return;
      }
      _socket = socket;
      _log('WebSocket connected: $eventsUri');
      onConnectionChanged?.call(true);
      socket.listen((message) {
        try {
          final payload = jsonDecode(message as String) as Map<String, dynamic>;
          final status = payload['status'];
          if (status is Map<String, dynamic>) onStatus?.call(status);
        } on Object catch (error) {
          onError?.call('invalid backend status: $error');
        }
      }, onError: (_) => _eventsDisconnected(), onDone: _eventsDisconnected,
          cancelOnError: true);
    } on Object catch (error) {
      _log('WebSocket connection failed: $error');
      _eventsDisconnected();
    }
  }

  void _eventsDisconnected() {
    if (_closed) return;
    _socket = null;
    _log('WebSocket disconnected; retrying in 2 seconds');
    onConnectionChanged?.call(false);
    _reconnect?.cancel();
    _reconnect = Timer(const Duration(seconds: 2), _connectEvents);
  }
}

class CameraBackendException implements Exception {
  CameraBackendException(this.message);
  final String message;
  @override
  String toString() => message;
}

Future<String> preserveArtifact(
  String sourcePath,
  String extension, {
  String directoryName = 'Pictures',
}) async {
  final pictures = Directory(
      '${Platform.environment['HOME'] ?? Directory.current.path}/$directoryName');
  await pictures.create(recursive: true);
  final now = DateTime.now();
  String two(int value) => value.toString().padLeft(2, '0');
  final name = 'RCAM_${two(now.day)}${two(now.month)}${now.year}_'
      '${two(now.hour)}${two(now.minute)}${two(now.second)}.$extension';
  final destination = File('${pictures.path}/$name');
  _log('preserve $extension: source=$sourcePath destination=${destination.path}');
  try {
    final source = File(sourcePath);
    final sourceStat = await source.stat();
    _log('source exists: type=${sourceStat.type} bytes=${sourceStat.size} '
        'mode=${sourceStat.mode.toRadixString(8)}');
    await source.copy(destination.path);
    final destinationStat = await destination.stat();
    _log('preserve succeeded: ${destination.path} bytes=${destinationStat.size}');
  } on Object catch (error) {
    _log('preserve failed: source=$sourcePath destination=${destination.path} '
        'error=$error');
    rethrow;
  }
  return destination.path;
}
