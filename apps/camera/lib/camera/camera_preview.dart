import 'dart:math' as math;
import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

const mockPreviewAsset = 'assets/mock_preview.png';

/// Replace this widget with the Linux video surface; keep interaction overlays.
class MockPreviewSurface extends StatelessWidget {
  const MockPreviewSurface({super.key});

  @override
  Widget build(BuildContext context) => Image.asset(
    mockPreviewAsset,
    fit: BoxFit.cover,
    width: double.infinity,
    height: double.infinity,
    excludeFromSemantics: true,
  );
}

class CameraPreview extends StatefulWidget {
  const CameraPreview({
    super.key,
    required this.focusPoint,
    required this.onFocus,
  });
  final Offset? focusPoint;
  final void Function(double x, double y) onFocus;

  @override
  State<CameraPreview> createState() => _CameraPreviewState();
}

class _CameraPreviewState extends State<CameraPreview> {
  static const _channel = MethodChannel('camera/native_preview');
  int? _textureId;
  int? _width;
  int? _height;

  @override
  void initState() {
    super.initState();
    if (Platform.isLinux) _startNativePreview();
  }

  Future<void> _startNativePreview() async {
    try {
      final result = await _channel.invokeMethod<Map<dynamic, dynamic>>('start');
      if (!mounted || result == null) return;
      setState(() {
        _textureId = (result['textureId'] as num).toInt();
        _width = (result['width'] as num).toInt();
        _height = (result['height'] as num).toInt();
      });
    } catch (error) {
      debugPrint('native camera preview unavailable: $error');
    }
  }

  @override
  void dispose() {
    if (Platform.isLinux) _channel.invokeMethod<void>('stop').catchError((_) {});
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) => Semantics(
      label: 'Rear camera preview. Tap to position focus.',
      child: GestureDetector(
        key: const Key('preview-touch'),
        behavior: HitTestBehavior.opaque,
          onTapDown: (details) => widget.onFocus(
          details.localPosition.dx / constraints.maxWidth,
          details.localPosition.dy / constraints.maxHeight,
        ),
        child: Stack(
          fit: StackFit.expand,
          clipBehavior: Clip.hardEdge,
          children: [
            if (_textureId case final textureId?)
              ClipRect(
                child: FittedBox(
                  fit: BoxFit.cover,
                  child: SizedBox(
                    width: _width?.toDouble() ?? 1920,
                    height: _height?.toDouble() ?? 1080,
                    child: Texture(
                      textureId: textureId,
                      filterQuality: FilterQuality.none,
                    ),
                  ),
                ),
              )
            else
              const MockPreviewSurface(),
            if (widget.focusPoint case final point?)
              Positioned(
                left: point.dx * constraints.maxWidth - 58,
                top: point.dy * constraints.maxHeight - 58,
                child: IgnorePointer(
                  child: TweenAnimationBuilder<double>(
                    key: ValueKey(point),
                    tween: Tween(begin: 1.2, end: 1),
                    duration: const Duration(milliseconds: 260),
                    builder: (context, scale, child) =>
                        Transform.scale(scale: scale, child: child),
                    child: const CustomPaint(
                      size: Size(116, 116),
                      painter: _FocusPainter(),
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
    ),
  );
}

class _FocusPainter extends CustomPainter {
  const _FocusPainter();
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Colors.white
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5;
    canvas.drawCircle(size.center(Offset.zero), 49, paint);
    for (var i = 0; i < 4; i++) {
      canvas.save();
      canvas.translate(58, 58);
      canvas.rotate(i * math.pi / 2);
      canvas.drawPath(
        Path()
          ..moveTo(-55, -46)
          ..lineTo(-55, -55)
          ..lineTo(-46, -55),
        paint,
      );
      canvas.restore();
    }
    canvas.drawCircle(
      size.center(Offset.zero),
      2,
      paint..style = PaintingStyle.fill,
    );
  }

  @override
  bool shouldRepaint(_FocusPainter oldDelegate) => false;
}
