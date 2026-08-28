using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Input;
using Tfrs.VoiceClient.Audio;
using Tfrs.VoiceClient.Localization;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

public partial class SettingsWindow : Window
{
    private readonly AppSettings _settings;
    private readonly VoiceSessionCoordinator _coordinator;
    private readonly Action _onHotkeysChanged;
    private readonly Action<float> _micLevelHandler;

    private enum BindTarget { None, Ptt, MicMute, SpeakerMute }
    private BindTarget _awaitingBind = BindTarget.None;

    // Mic level events fire ~50/s while connected — same flooding risk MainWindow already
    // throttles (see its ShouldThrottledUpdate doc comment): without this, opening Settings while
    // connected queues a Dispatcher.BeginInvoke faster than the UI thread drains them, which is
    // exactly the "Settings menu lags" symptom.
    private const long LevelMeterThrottleMs = 50;
    private long _lastMicLevelUpdateTicks;

    internal SettingsWindow(AppSettings settings, VoiceSessionCoordinator coordinator,
        ObservableCollection<string> logEntries, Action onHotkeysChanged)
    {
        _settings = settings;
        _coordinator = coordinator;
        _onHotkeysChanged = onHotkeysChanged;
        InitializeComponent();
        LogListBox.ItemsSource = logEntries;
        PreviewKeyDown += SettingsWindow_PreviewKeyDown;
        Loaded += SettingsWindow_Loaded;
        Closed += SettingsWindow_Closed;

        // Mirrors the main window's mic level meter next to the VAD threshold slider, so you can
        // see where your voice sits relative to it while tuning — same source event, just a
        // second subscriber (multicast delegate, no conflict with MainWindow's own subscription).
        _micLevelHandler = level =>
        {
            long now = Environment.TickCount64;
            if (now - _lastMicLevelUpdateTicks < LevelMeterThrottleMs) return;
            _lastMicLevelUpdateTicks = now;
            Dispatcher.BeginInvoke(() => VadLevelBar.Value = Math.Min(level, VadLevelBar.Maximum));
        };
        _coordinator.MicLevelMeasured += _micLevelHandler;
    }

    private void SettingsWindow_Closed(object? sender, EventArgs e) =>
        _coordinator.MicLevelMeasured -= _micLevelHandler;

    private void SettingsWindow_Loaded(object sender, RoutedEventArgs e)
    {
        PopulateDevices();
        MicVolumeSlider.Value = _settings.MicVolume;
        SpeakerVolumeSlider.Value = _settings.SpeakerVolume;
        VadThresholdSlider.Value = _settings.VadThreshold;
        SetModeRadio(_settings.TransmitMode);
        UpdateBindButtonLabels();
    }

    private void PopulateDevices()
    {
        // Id = null means "follow the OS default" (AudioDevices.Resolve already treats a null/
        // empty id that way) — listed first so it's also what a fresh install lands on.
        string systemDefault = Loc.Get("SystemDefault");

        InputDeviceCombo.Items.Clear();
        InputDeviceCombo.Items.Add(new DeviceItem(null, systemDefault));
        foreach (var d in AudioDevices.ListInputs())
            InputDeviceCombo.Items.Add(new DeviceItem(d.Id, d.Name));
        SelectMatching(InputDeviceCombo, _settings.InputDeviceId);

        OutputDeviceCombo.Items.Clear();
        OutputDeviceCombo.Items.Add(new DeviceItem(null, systemDefault));
        foreach (var d in AudioDevices.ListOutputs())
            OutputDeviceCombo.Items.Add(new DeviceItem(d.Id, d.Name));
        SelectMatching(OutputDeviceCombo, _settings.OutputDeviceId);
    }

    private static void SelectMatching(System.Windows.Controls.ComboBox combo, string? id)
    {
        foreach (var obj in combo.Items)
        {
            if (obj is DeviceItem item && item.Id == id) { combo.SelectedItem = item; return; }
        }
        if (combo.Items.Count > 0) combo.SelectedIndex = 0;
    }

    private sealed record DeviceItem(string? Id, string Name)
    {
        public override string ToString() => Name;
    }

    private void InputDeviceCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (InputDeviceCombo.SelectedItem is DeviceItem item) _coordinator.SetInputDevice(item.Id);
    }

    private void OutputDeviceCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (OutputDeviceCombo.SelectedItem is DeviceItem item) _coordinator.SetOutputDevice(item.Id);
    }

    private void MicVolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        _coordinator.MicVolume = (float)e.NewValue;

    private void SpeakerVolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        _coordinator.SpeakerVolume = (float)e.NewValue;

    private void VadThresholdSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        _coordinator.VadThreshold = (float)e.NewValue;

    private void SetModeRadio(TransmitMode mode)
    {
        PttRadio.IsChecked = mode == TransmitMode.PushToTalk;
        VadRadio.IsChecked = mode == TransmitMode.VoiceActivation;
        AlwaysOnRadio.IsChecked = mode == TransmitMode.AlwaysOn;
    }

    private void TransmitModeRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (ReferenceEquals(sender, PttRadio)) _coordinator.TransmitMode = TransmitMode.PushToTalk;
        else if (ReferenceEquals(sender, VadRadio)) _coordinator.TransmitMode = TransmitMode.VoiceActivation;
        else if (ReferenceEquals(sender, AlwaysOnRadio)) _coordinator.TransmitMode = TransmitMode.AlwaysOn;
    }

    private void PttBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.Ptt, PttBindButton);
    private void MicMuteBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.MicMute, MicMuteBindButton);
    private void SpeakerMuteBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.SpeakerMute, SpeakerMuteBindButton);

    private void BeginBind(BindTarget target, System.Windows.Controls.Button button)
    {
        _awaitingBind = target;
        button.Content = Loc.Get("PressAKey");
        Keyboard.Focus(this);
    }

    private void SettingsWindow_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (_awaitingBind == BindTarget.None) return;
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key == Key.None) return;

        int vKey = KeyInterop.VirtualKeyFromKey(key);
        var target = _awaitingBind;
        _awaitingBind = BindTarget.None;
        e.Handled = true;

        switch (target)
        {
            case BindTarget.Ptt:
                _settings.PttKey = vKey;
                break;
            case BindTarget.MicMute:
                _settings.MicMuteKey = vKey;
                break;
            case BindTarget.SpeakerMute:
                _settings.SpeakerMuteKey = vKey;
                break;
        }
        UpdateBindButtonLabels();
        _onHotkeysChanged();
    }

    private void UpdateBindButtonLabels()
    {
        PttBindButton.Content = DescribeKey(_settings.PttKey);
        MicMuteBindButton.Content = DescribeKey(_settings.MicMuteKey);
        SpeakerMuteBindButton.Content = DescribeKey(_settings.SpeakerMuteKey);
    }

    private static string DescribeKey(int vKey) =>
        vKey == 0 ? Loc.Get("NotBound") : KeyInterop.KeyFromVirtualKey(vKey).ToString();

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();
}
