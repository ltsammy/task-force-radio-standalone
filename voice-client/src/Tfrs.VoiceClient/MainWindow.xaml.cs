using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using Tfrs.VoiceClient.Audio;
using Tfrs.VoiceClient.Hotkeys;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

public partial class MainWindow : Window
{
    private readonly AppSettings _settings;
    private readonly CommandLineArgs _cliArgs;
    private VoiceSessionCoordinator? _coordinator;
    private PttKeyPoller? _pttPoller;
    private GlobalHotkeyManager? _hotkeys;
    private int? _micMuteHotkeyId;
    private int? _speakerMuteHotkeyId;

    private enum BindTarget { None, Ptt, MicMute, SpeakerMute }
    private BindTarget _awaitingBind = BindTarget.None;

    internal MainWindow(AppSettings settings, CommandLineArgs cliArgs)
    {
        _settings = settings;
        _cliArgs = cliArgs;
        InitializeComponent();
        Loaded += MainWindow_Loaded;
        Closing += MainWindow_Closing;
        PreviewKeyDown += MainWindow_PreviewKeyDown;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        HostTextBox.Text = _cliArgs.Host ?? _settings.ServerHost;
        PortTextBox.Text = (_cliArgs.Port ?? _settings.ServerPort).ToString();
        PasswordBox.Password = _cliArgs.Password ?? _settings.ServerPassword;
        if (!string.IsNullOrWhiteSpace(_cliArgs.Uid)) _settings.LocalUid = _cliArgs.Uid;

        // Launched by an external launcher with connection details already decided — an average
        // player doesn't need to see or edit the server IP/port/password.
        if (!string.IsNullOrWhiteSpace(_cliArgs.Host))
            ServerGroupBox.Visibility = Visibility.Collapsed;

        PopulateDevices();
        MicVolumeSlider.Value = _settings.MicVolume;
        SpeakerVolumeSlider.Value = _settings.SpeakerVolume;
        VadThresholdSlider.Value = _settings.VadThreshold;
        SetModeRadio(_settings.TransmitMode);
        UpdateBindButtonLabels();

        _coordinator = new VoiceSessionCoordinator(_settings);
        _coordinator.ConnectionStateChanged += connected => Dispatcher.BeginInvoke(() => OnConnectionStateChanged(connected));
        _coordinator.ConnectError += msg => Dispatcher.BeginInvoke(() => Log($"Verbindung fehlgeschlagen: {msg}"));
        _coordinator.TransmittingChanged += t => Dispatcher.BeginInvoke(() => StatusText.Text = t ? "Verbunden — sendet…" : "Verbunden");
        _coordinator.MicLevelMeasured += level => Dispatcher.BeginInvoke(() => MicLevelBar.Value = Math.Min(level, MicLevelBar.Maximum));
        _coordinator.SpeakerLevelMeasured += level => Dispatcher.BeginInvoke(() => SpeakerLevelBar.Value = Math.Min(level, SpeakerLevelBar.Maximum));
        _coordinator.ExtensionConnectionChanged += connected => Dispatcher.BeginInvoke(() =>
            ExtensionStatusText.Text = connected ? "Arma-Addon: verbunden" : "Arma-Addon: nicht verbunden");
        _coordinator.ConnectedCountChanged += count => Dispatcher.BeginInvoke(() =>
            RosterCountText.Text = count > 0 ? $"Verbundene Clients: {count}" : "");

        var windowHandle = new WindowInteropHelper(this).EnsureHandle();
        _hotkeys = new GlobalHotkeyManager(windowHandle);
        _pttPoller = new PttKeyPoller();
        _pttPoller.HeldChanged += held => _coordinator?.SetPttHeld(held);
        _pttPoller.SetKey(_settings.PttKey);
        _pttPoller.Start();
        ApplyMuteHotkeys();

        if (_cliArgs.AutoConnect && !string.IsNullOrWhiteSpace(HostTextBox.Text))
            await ConnectAsync();
    }

    private async void MainWindow_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _settings.ServerHost = HostTextBox.Text;
        if (int.TryParse(PortTextBox.Text, out int port)) _settings.ServerPort = port;
        _settings.ServerPassword = PasswordBox.Password;
        _settings.Save();

        _pttPoller?.Dispose();
        _hotkeys?.Dispose();
        if (_coordinator is not null) await _coordinator.DisposeAsync();
    }

    private void PopulateDevices()
    {
        // Id = null means "follow the OS default" (AudioDevices.Resolve already treats a null/
        // empty id that way) — listed first so it's also what a fresh install lands on.
        InputDeviceCombo.Items.Clear();
        InputDeviceCombo.Items.Add(new DeviceItem(null, "Systemstandard"));
        foreach (var d in AudioDevices.ListInputs())
            InputDeviceCombo.Items.Add(new DeviceItem(d.Id, d.Name));
        SelectMatching(InputDeviceCombo, _settings.InputDeviceId);

        OutputDeviceCombo.Items.Clear();
        OutputDeviceCombo.Items.Add(new DeviceItem(null, "Systemstandard"));
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

    private async void ConnectButton_Click(object sender, RoutedEventArgs e) => await ConnectAsync();

    private async Task ConnectAsync()
    {
        if (_coordinator is null) return;
        if (!int.TryParse(PortTextBox.Text, out int port))
        {
            Log("Ungültiger Port.");
            return;
        }

        ConnectButton.IsEnabled = false;
        // No user-facing display name (see project notes): the relay only needs a UID for
        // routing, so a fixed technical name is fine — nothing shows it in the UI.
        bool ok = await _coordinator.ConnectAsync(HostTextBox.Text.Trim(), port, PasswordBox.Password, "TFRS");
        ConnectButton.IsEnabled = !ok;
        if (ok) Log($"Verbunden mit {HostTextBox.Text}:{port}.");
    }

    private async void DisconnectButton_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator is null) return;
        await _coordinator.DisconnectAsync();
        Log("Getrennt.");
    }

    private void OnConnectionStateChanged(bool connected)
    {
        ConnectButton.IsEnabled = !connected;
        DisconnectButton.IsEnabled = connected;
        StatusText.Text = connected ? "Verbunden" : "Nicht verbunden";
    }

    private void InputDeviceCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (InputDeviceCombo.SelectedItem is DeviceItem item) _coordinator?.SetInputDevice(item.Id);
    }

    private void OutputDeviceCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (OutputDeviceCombo.SelectedItem is DeviceItem item) _coordinator?.SetOutputDevice(item.Id);
    }

    private void MicVolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_coordinator is not null) _coordinator.MicVolume = (float)e.NewValue;
    }

    private void SpeakerVolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_coordinator is not null) _coordinator.SpeakerVolume = (float)e.NewValue;
    }

    private void VadThresholdSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_coordinator is not null) _coordinator.VadThreshold = (float)e.NewValue;
    }

    private void SetModeRadio(TransmitMode mode)
    {
        PttRadio.IsChecked = mode == TransmitMode.PushToTalk;
        VadRadio.IsChecked = mode == TransmitMode.VoiceActivation;
        AlwaysOnRadio.IsChecked = mode == TransmitMode.AlwaysOn;
    }

    private void TransmitModeRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (_coordinator is null) return;
        if (ReferenceEquals(sender, PttRadio)) _coordinator.TransmitMode = TransmitMode.PushToTalk;
        else if (ReferenceEquals(sender, VadRadio)) _coordinator.TransmitMode = TransmitMode.VoiceActivation;
        else if (ReferenceEquals(sender, AlwaysOnRadio)) _coordinator.TransmitMode = TransmitMode.AlwaysOn;
    }

    private void MicMuteToggle_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator is not null) _coordinator.MicMuted = MicMuteToggle.IsChecked == true;
    }

    private void SpeakerMuteToggle_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator is not null) _coordinator.SpeakerMuted = SpeakerMuteToggle.IsChecked == true;
    }

    private void PttBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.Ptt, PttBindButton);
    private void MicMuteBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.MicMute, MicMuteBindButton);
    private void SpeakerMuteBindButton_Click(object sender, RoutedEventArgs e) => BeginBind(BindTarget.SpeakerMute, SpeakerMuteBindButton);

    private void BeginBind(BindTarget target, System.Windows.Controls.Button button)
    {
        _awaitingBind = target;
        button.Content = "Taste drücken…";
        Keyboard.Focus(this);
    }

    private void MainWindow_PreviewKeyDown(object sender, KeyEventArgs e)
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
                _pttPoller?.SetKey(vKey);
                break;
            case BindTarget.MicMute:
                _settings.MicMuteKey = vKey;
                ApplyMuteHotkeys();
                break;
            case BindTarget.SpeakerMute:
                _settings.SpeakerMuteKey = vKey;
                ApplyMuteHotkeys();
                break;
        }
        UpdateBindButtonLabels();
    }

    private void ApplyMuteHotkeys()
    {
        if (_hotkeys is null) return;

        if (_micMuteHotkeyId is int oldMic) { _hotkeys.Unregister(oldMic); _micMuteHotkeyId = null; }
        if (_speakerMuteHotkeyId is int oldSpk) { _hotkeys.Unregister(oldSpk); _speakerMuteHotkeyId = null; }

        try
        {
            if (_settings.MicMuteKey != 0)
                _micMuteHotkeyId = _hotkeys.Register(_settings.MicMuteKey, () => Dispatcher.BeginInvoke(() =>
                {
                    MicMuteToggle.IsChecked = !(MicMuteToggle.IsChecked == true);
                    if (_coordinator is not null) _coordinator.MicMuted = MicMuteToggle.IsChecked == true;
                }));
            if (_settings.SpeakerMuteKey != 0)
                _speakerMuteHotkeyId = _hotkeys.Register(_settings.SpeakerMuteKey, () => Dispatcher.BeginInvoke(() =>
                {
                    SpeakerMuteToggle.IsChecked = !(SpeakerMuteToggle.IsChecked == true);
                    if (_coordinator is not null) _coordinator.SpeakerMuted = SpeakerMuteToggle.IsChecked == true;
                }));
        }
        catch (InvalidOperationException ex)
        {
            Log(ex.Message);
        }
    }

    private void UpdateBindButtonLabels()
    {
        PttBindButton.Content = DescribeKey(_settings.PttKey);
        MicMuteBindButton.Content = DescribeKey(_settings.MicMuteKey);
        SpeakerMuteBindButton.Content = DescribeKey(_settings.SpeakerMuteKey);
    }

    private static string DescribeKey(int vKey) =>
        vKey == 0 ? "(nicht gebunden)" : KeyInterop.KeyFromVirtualKey(vKey).ToString();

    private void Log(string message) => LogListBox.Items.Insert(0, $"{DateTime.Now:HH:mm:ss}  {message}");
}
