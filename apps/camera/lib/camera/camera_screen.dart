import 'package:flutter/material.dart';
import 'camera_backend.dart';
import 'camera_preview.dart';
import 'camera_state.dart';

const _blue = Color(0xFF347BFA);
const _panel = Color(0xEF141718);

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key, this.backend});
  final CameraBackend? backend;
  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  late final CameraUiState state = CameraUiState(backend: widget.backend);
  @override
  void dispose() {
    state.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: SafeArea(
      child: ListenableBuilder(
        listenable: state,
        builder: (context, _) => Row(
          children: [
            ManualRail(state: state),
            Expanded(
              child: Stack(
                fit: StackFit.expand,
                children: [
                  CameraPreview(
                    focusPoint: state.focusPoint,
                    onFocus: state.requestFocus,
                  ),
                  Positioned(
                    top: 16,
                    left: 16,
                    child: IgnorePointer(
                      child: _Badge(
                        icon: state.recording
                            ? Icons.fiber_manual_record
                            : Icons.image_outlined,
                        text: state.mode == CameraMode.photo
                            ? '12 MP  ·  ${state.format.name.toUpperCase()}'
                            : state.recording
                            ? 'REC  ·  ${state.backendConnected ? state.recordingState.toUpperCase() : 'SIMULATED'}'
                            : 'VIDEO  ·  STANDBY',
                        recording: state.recording,
                      ),
                    ),
                  ),
                  const Positioned(
                    top: 16,
                    right: 16,
                    child: IgnorePointer(
                      child: _Badge(text: 'REAR  ·  LIVE PREVIEW'),
                    ),
                  ),
                  if (state.selectedControl != null)
                    Positioned(
                      left: 16,
                      bottom: 20,
                      right: 16,
                      child: Align(
                        alignment: Alignment.bottomLeft,
                        child: ManualControlPanel(state: state),
                      ),
                    ),
                  if (state.errorMessage != null)
                    Positioned(
                      left: 16,
                      right: 16,
                      top: 56,
                      child: IgnorePointer(
                        child: DecoratedBox(
                          decoration: const BoxDecoration(color: Colors.black87),
                          child: Padding(
                            padding: const EdgeInsets.all(8),
                            child: Text(state.errorMessage!),
                          ),
                        ),
                      ),
                    ),
                ],
              ),
            ),
            ActionRail(state: state),
          ],
        ),
      ),
    ),
  );
}

class ManualRail extends StatelessWidget {
  const ManualRail({super.key, required this.state});
  final CameraUiState state;

  @override
  Widget build(BuildContext context) => Container(
    width: 88,
    color: const Color(0xFF242A2E),
    child: LayoutBuilder(
      builder: (context, constraints) => SingleChildScrollView(
        child: ConstrainedBox(
          constraints: BoxConstraints(minHeight: constraints.maxHeight),
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 18, horizontal: 9),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                if (state.controlsExpanded) ...[
                  _control(
                    ManualControl.af,
                    Icons.my_location,
                    'AF',
                    state.autofocus ? 'Auto' : 'Manual',
                    active: state.autofocus,
                  ),
                  _control(
                    ManualControl.ev,
                    Icons.exposure,
                    '${state.ev >= 0 ? '+' : ''}${state.ev.toStringAsFixed(1)}',
                    'EV',
                  ),
                  _control(
                    ManualControl.iso,
                    Icons.iso_outlined,
                    'ISO',
                    CameraUiState.isoValues[state.isoIndex],
                  ),
                  _control(
                    ManualControl.exposure,
                    Icons.camera,
                    CameraUiState.exposureValues[state.exposureIndex],
                    'S',
                  ),
                  _control(
                    ManualControl.focus,
                    Icons.center_focus_strong,
                    'MF',
                    state.manualFocus == null
                        ? 'Auto'
                        : '${(state.manualFocus! * 100).round()}%',
                  ),
                  _control(
                    ManualControl.locks,
                    state.locked
                        ? Icons.lock_outline
                        : Icons.lock_open_outlined,
                    'AE/AWB',
                    state.locked ? 'Locked' : 'Off',
                    active: state.locked,
                  ),
                ],
                Padding(
                  padding: const EdgeInsets.only(top: 14),
                  child: IconButton.filledTonal(
                    key: const Key('toggle-controls'),
                    constraints: const BoxConstraints.tightFor(
                      width: 54,
                      height: 54,
                    ),
                    style: IconButton.styleFrom(
                      foregroundColor: Colors.white,
                      backgroundColor: const Color(0xFF171B1D),
                    ),
                    tooltip: state.controlsExpanded
                        ? 'Collapse controls'
                        : 'Expand controls',
                    onPressed: state.toggleControls,
                    icon: Icon(
                      state.controlsExpanded
                          ? Icons.keyboard_arrow_up
                          : Icons.keyboard_arrow_down,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    ),
  );

  Widget _control(
    ManualControl control,
    IconData icon,
    String label,
    String value, {
    bool active = false,
  }) {
    final selected = state.selectedControl == control;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Semantics(
        selected: selected,
        child: TextButton(
          key: Key('control-${control.name}'),
          onPressed: () => state.selectControl(control),
          style: TextButton.styleFrom(
            foregroundColor: Colors.white,
            backgroundColor: const Color(0xFF171B1D),
            minimumSize: const Size(70, 78),
            padding: const EdgeInsets.symmetric(vertical: 10, horizontal: 2),
            shape: const StadiumBorder(),
            side: BorderSide(
              color: selected || active ? _blue : Colors.transparent,
              width: 1.5,
            ),
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(icon, size: 22),
              const SizedBox(height: 5),
              Text(label, style: const TextStyle(fontSize: 13)),
              Text(
                value,
                style: TextStyle(
                  fontSize: 12,
                  color: selected || active
                      ? const Color(0xFF83BFFF)
                      : Colors.white70,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class ActionRail extends StatelessWidget {
  const ActionRail({super.key, required this.state});
  final CameraUiState state;

  @override
  Widget build(BuildContext context) => Container(
    width: 168,
    color: const Color(0xFF0B0D0E),
    child: LayoutBuilder(
      builder: (context, constraints) => SingleChildScrollView(
        child: SizedBox(
          height: constraints.maxHeight < 560 ? 560 : constraints.maxHeight,
          child: Column(
            children: [
              const Spacer(flex: 3),
              SizedBox(
                height: 48,
                child: state.mode == CameraMode.photo
                    ? _Selector<PhotoFormat>(
                        values: PhotoFormat.values,
                        selected: state.format,
                        label: (v) => v.name.toUpperCase(),
                        onSelected: state.selectFormat,
                      )
                    : const Center(
                        child: Text(
                          'VIDEO',
                          style: TextStyle(
                            fontSize: 12,
                            letterSpacing: 2,
                            color: Colors.white54,
                          ),
                        ),
                      ),
              ),
              const Spacer(flex: 3),
              Semantics(
                label: 'Take photo',
                button: true,
                enabled: state.mode == CameraMode.photo,
                child: IconButton(
                  key: const Key('shutter'),
                  tooltip: 'Take photo',
                  onPressed: state.mode == CameraMode.photo
                      ? state.capture
                      : null,
                  padding: EdgeInsets.zero,
                  icon: Opacity(
                    opacity: state.mode == CameraMode.photo ? 1 : .25,
                    child: Container(
                      width: 100,
                      height: 100,
                      padding: const EdgeInsets.all(5),
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        border: Border.all(color: Colors.white, width: 2),
                      ),
                      child: const DecoratedBox(
                        decoration: BoxDecoration(
                          color: Color(0xFFF7F8FA),
                          shape: BoxShape.circle,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 24),
              IconButton(
                key: const Key('record'),
                tooltip: state.recording
                    ? 'Stop recording'
                    : 'Start recording',
                onPressed: state.toggleRecording,
                padding: EdgeInsets.zero,
                icon: Container(
                  width: 64,
                  height: 64,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    border: Border.all(
                      color: state.recording
                          ? Colors.redAccent
                          : Colors.white24,
                      width: 2,
                    ),
                  ),
                  child: Center(
                    child: AnimatedContainer(
                      duration: const Duration(milliseconds: 180),
                      width: state.recording ? 22 : 20,
                      height: state.recording ? 22 : 20,
                      decoration: BoxDecoration(
                        color: const Color(0xFFF13737),
                        borderRadius: BorderRadius.circular(
                          state.recording ? 4 : 20,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
              const Spacer(flex: 2),
              _Selector<CameraMode>(
                values: CameraMode.values,
                selected: state.mode,
                label: (v) => v == CameraMode.photo ? 'Photo' : 'Video',
                onSelected: state.recording ? null : state.selectMode,
              ),
              const Spacer(),
              Tooltip(
                message: state.recentShot == 0
                    ? 'Recent shot placeholder'
                    : 'Mock photo ${state.recentShot} · ${state.recentFormat!.name.toUpperCase()} · not saved',
                child: Semantics(
                  label: state.recentShot == 0
                      ? 'Recent shot placeholder'
                      : 'Mock photo ${state.recentShot}, not saved',
                  child: Container(
                    key: ValueKey('recent-${state.recentShot}'),
                    width: 104,
                    height: 104,
                    clipBehavior: Clip.antiAlias,
                    decoration: BoxDecoration(
                      borderRadius: BorderRadius.circular(10),
                      border: Border.all(color: Colors.white70),
                    ),
                    child: Stack(
                      fit: StackFit.expand,
                      children: [
                        Image.asset(mockPreviewAsset, fit: BoxFit.cover),
                        Positioned(
                          bottom: 0,
                          left: 0,
                          right: 0,
                          child: ColoredBox(
                            color: Colors.black54,
                            child: Padding(
                              padding: const EdgeInsets.all(4),
                              child: Text(
                                state.recentShot == 0
                                    ? 'SAMPLE'
                                    : 'MOCK ${state.recentShot}',
                                textAlign: TextAlign.center,
                                style: const TextStyle(
                                  fontSize: 10,
                                  letterSpacing: 1,
                                ),
                              ),
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 28),
            ],
          ),
        ),
      ),
    ),
  );
}

class ManualControlPanel extends StatelessWidget {
  const ManualControlPanel({super.key, required this.state});
  final CameraUiState state;

  @override
  Widget build(BuildContext context) {
    final control = state.selectedControl!;
    final title = switch (control) {
      ManualControl.af => 'Autofocus',
      ManualControl.ev => 'Exposure compensation',
      ManualControl.iso => 'Sensitivity · ISO',
      ManualControl.exposure => 'Exposure time',
      ManualControl.focus => 'Manual focus',
      ManualControl.locks => 'AE / AWB lock',
    };
    final body = switch (control) {
      ManualControl.af => _choices(
        ['Auto', 'Manual'],
        state.autofocus ? 0 : 1,
        (i) => state.updateManual(() {
          state.autofocus = i == 0;
          state.manualFocus = i == 0 ? null : .5;
        }),
      ),
      ManualControl.ev => Slider(
        value: state.ev,
        min: -3,
        max: 3,
        divisions: 12,
        label: '${state.ev.toStringAsFixed(1)} EV',
        onChanged: (v) => state.updateManual(() => state.ev = v),
      ),
      ManualControl.iso => _choices(
        CameraUiState.isoValues,
        state.isoIndex,
        (i) => state.updateManual(() => state.isoIndex = i),
      ),
      ManualControl.exposure => _choices(
        CameraUiState.exposureValues,
        state.exposureIndex,
        (i) => state.updateManual(() => state.exposureIndex = i),
      ),
      ManualControl.focus => Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          _choices(
            ['Auto', 'Manual'],
            state.manualFocus == null ? 0 : 1,
            (i) => state.updateManual(() {
              state.manualFocus = i == 0 ? null : .5;
              state.autofocus = i == 0;
            }),
          ),
          Slider(
            value: state.manualFocus ?? .5,
            label: 'Focus ${(100 * (state.manualFocus ?? .5)).round()}%',
            onChanged: (v) => state.updateManual(() {
              state.manualFocus = v;
              state.autofocus = false;
            }),
          ),
          const Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [Text('Near'), Text('Far')],
          ),
        ],
      ),
      ManualControl.locks => _choices(
        ['Off', 'Locked'],
        state.locked ? 1 : 0,
        (i) => state.updateManual(() => state.locked = i == 1),
      ),
    };
    return Container(
      key: const Key('manual-panel'),
      width: 380,
      padding: const EdgeInsets.fromLTRB(18, 6, 10, 16),
      decoration: BoxDecoration(
        color: _panel,
        borderRadius: BorderRadius.circular(18),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  title,
                  style: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w500,
                  ),
                ),
              ),
              IconButton(
                tooltip: 'Close control',
                onPressed: () => state.selectControl(control),
                icon: const Icon(Icons.close, size: 20),
              ),
            ],
          ),
          body,
          const SizedBox(height: 8),
          const Text(
            'UI simulation',
            style: TextStyle(fontSize: 11, color: Colors.white38),
          ),
        ],
      ),
    );
  }

  Widget _choices(
    List<String> values,
    int selected,
    ValueChanged<int> onSelected,
  ) => Wrap(
    spacing: 6,
    runSpacing: 6,
    children: [
      for (var i = 0; i < values.length; i++)
        ChoiceChip(
          label: Text(values[i]),
          selected: i == selected,
          showCheckmark: false,
          selectedColor: _blue,
          onSelected: (_) => onSelected(i),
          materialTapTargetSize: MaterialTapTargetSize.padded,
        ),
    ],
  );
}

class _Selector<T> extends StatelessWidget {
  const _Selector({
    required this.values,
    required this.selected,
    required this.label,
    required this.onSelected,
  });
  final List<T> values;
  final T selected;
  final String Function(T) label;
  final ValueChanged<T>? onSelected;

  @override
  Widget build(BuildContext context) => Container(
    margin: const EdgeInsets.symmetric(horizontal: 10),
    padding: const EdgeInsets.all(3),
    decoration: BoxDecoration(
      color: const Color(0xFF121415),
      borderRadius: BorderRadius.circular(28),
      border: Border.all(color: Colors.white24),
    ),
    child: Row(
      children: [
        for (final value in values)
          Expanded(
            child: Semantics(
              selected: selected == value,
              child: TextButton(
                onPressed: onSelected == null ? null : () => onSelected!(value),
                style: TextButton.styleFrom(
                  minimumSize: const Size(48, 42),
                  padding: EdgeInsets.zero,
                  foregroundColor: Colors.white,
                  disabledForegroundColor: Colors.white38,
                  backgroundColor: selected == value
                      ? _blue
                      : Colors.transparent,
                  shape: const StadiumBorder(),
                ),
                child: Text(label(value), style: const TextStyle(fontSize: 14)),
              ),
            ),
          ),
      ],
    ),
  );
}

class _Badge extends StatelessWidget {
  const _Badge({required this.text, this.icon, this.recording = false});
  final String text;
  final IconData? icon;
  final bool recording;
  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
    decoration: BoxDecoration(
      color: const Color(0xA6262D33),
      borderRadius: BorderRadius.circular(22),
    ),
    child: Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (icon != null) ...[
          Icon(
            icon,
            size: 19,
            color: recording ? Colors.redAccent : Colors.white,
          ),
          const SizedBox(width: 8),
        ],
        Text(
          text,
          style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w500),
        ),
      ],
    ),
  );
}
