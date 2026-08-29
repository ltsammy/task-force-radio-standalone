using System.Collections.ObjectModel;
using System.Windows;
using Tfrs.VoiceClient.Audio;
using Tfrs.VoiceClient.Hotkeys;
using Tfrs.VoiceClient.Localization;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

public partial class MainWindow : Window
{
    private readonly AppSettings _settings;
    private readonly CommandLineArgs _cliArgs;
    private readonly ObservableCollection<string> _logEntries = new();
    private VoiceSessionCoordinator? _coordinator;
    private readonly KeyPoller _pttPoller = new();
    private readonly KeyPoller _micMutePoller = new();
    private readonly KeyPoller _speakerMutePoller = new();
    private SettingsWindow? _settingsWindow;

    private const long LevelMeterThrottleMs = 50; // ~20fps — smooth enough, far less Dispatcher load
    private long _lastMicLevelUpdateTicks;
    private long _lastSpeakerLevelUpdateTicks;

    // Guards against AddonUidUpdated firing while a ConnectAsync handshake (up to ~7.5s across
    // retries) is already in flight: the in-flight attempt already read Settings.LocalUid at its
    // own start and can't be redirected mid-flight, so instead of racing a second concurrent
    // ConnectAsync against it (VoiceNetworkClient isn't safe against that), the correction is
    // deferred and replayed once the in-flight attempt finishes.
    private bool _connecting;
    private bool _identityChangedDuringConnect;
    private CancellationTokenSource? _autoReconnectCts;

    /// <summary>Called from a background audio thread — deliberately not synchronized (a torn
    /// read/write here just means one throttle window is occasionally a few ms off, which is
    /// irrelevant for a meter update rate; not worth a lock on a hot audio-callback path).</summary>
    private static bool ShouldThrottledUpdate(ref long lastTicks)
    {
        long now = Environment.TickCount64;
        if (now - lastTicks < LevelMeterThrottleMs) return false;
        lastTicks = now;
        return true;
    }

    internal MainWindow(AppSettings settings, CommandLineArgs cliArgs)
    {
        _settings = settings;
        _cliArgs = cliArgs;
        InitializeComponent();
        Loaded += MainWindow_Loaded;
        Closing += MainWindow_Closing;
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

        UpdateMuteIndicators();

        string clientVersion = System.Reflection.Assembly.GetExecutingAssembly().GetName().Version?.ToString() ?? "unknown";
        Log($"Client version: {clientVersion}");

        _coordinator = new VoiceSessionCoordinator(_settings);
        _coordinator.ConnectionStateChanged += connected => Dispatcher.BeginInvoke(() => OnConnectionStateChanged(connected));
        _coordinator.ConnectError += msg => Dispatcher.BeginInvoke(() => Log(Loc.Format("ConnectFailed", msg)));
        _coordinator.TransmittingChanged += t => Dispatcher.BeginInvoke(() => StatusText.Text = Loc.Get(t ? "StatusConnectedSending" : "StatusConnected"));
        // Level meters fire per audio frame (~50/s mic, often faster for playback) — every single
        // one becoming its own BeginInvoke can queue up on the UI thread faster than it drains,
        // which starves real input (clicks) behind a growing backlog: exactly "the UI freezes,
        // but only while connected" (connecting is what starts these events flowing at all).
        // Throttling to a still-smooth ~20fps cuts that queue by 60-80% for a meter nobody needs
        // sub-frame-accurate anyway.
        _coordinator.MicLevelMeasured += level =>
        {
            if (!ShouldThrottledUpdate(ref _lastMicLevelUpdateTicks)) return;
            Dispatcher.BeginInvoke(() => MicLevelBar.Value = Math.Min(level, MicLevelBar.Maximum));
        };
        _coordinator.SpeakerLevelMeasured += level =>
        {
            if (!ShouldThrottledUpdate(ref _lastSpeakerLevelUpdateTicks)) return;
            Dispatcher.BeginInvoke(() => SpeakerLevelBar.Value = Math.Min(level, SpeakerLevelBar.Maximum));
        };
        _coordinator.MicPersistentSilence += () => Dispatcher.BeginInvoke(() =>
        {
            Log(Loc.Get("SilenceHint"));
            SilenceHintText.Text = Loc.Get("SilenceHintShort");
            SilenceHintText.Visibility = Visibility.Visible;
        });
        // Fires immediately on connect (not after a 3s guess) whenever the resolved mic is muted
        // or at zero volume at the Windows endpoint level — a completely different setting from
        // the microphone privacy toggle, and the more common real-world cause of "no signal at all".
        _coordinator.MicDeviceMuted += () => Dispatcher.BeginInvoke(() =>
        {
            Log(Loc.Get("MicMutedAtOsHint"));
            SilenceHintText.Text = Loc.Get("MicMutedAtOsHint");
            SilenceHintText.Visibility = Visibility.Visible;
        });
        _coordinator.ExtensionConnectionChanged += connected => Dispatcher.BeginInvoke(() =>
        {
            ExtensionStatusText.Text = Loc.Get(connected ? "ExtensionConnected" : "ExtensionNotConnected");
            Log(connected ? "Arma addon: connected" : "Arma addon: disconnected");
        });
        _coordinator.ConnectedCountChanged += count => Dispatcher.BeginInvoke(() =>
            RosterCountText.Text = count > 0 ? Loc.Format("ConnectedCount", count) : "");
        _coordinator.AddonUidUpdated += () => Dispatcher.BeginInvoke(OnAddonUidUpdated);
        _coordinator.AddonOverrideChanged += v => Dispatcher.BeginInvoke(() => Log(v switch
        {
            false => "!!! Addon is forcing SILENCE (transmit override) — blocks ALL transmission, including Always-On.",
            true => "Addon is forcing transmission (PTT override) regardless of mode.",
            null => "Addon transmit override cleared — normal PTT/VAD/Always-On applies.",
        }));
        _coordinator.AddonVersionReceived += v => Dispatcher.BeginInvoke(() => Log($"Addon version: {v}"));
        _coordinator.BridgeError += msg => Dispatcher.BeginInvoke(() => Log($"!!! Addon bridge error (recovered): {msg}"));
        _coordinator.ConnectionLostUnexpectedly += () => Dispatcher.BeginInvoke(() =>
        {
            Log("Connection to server lost — retrying...");
            _ = AutoReconnectLoopAsync();
        });

        _pttPoller.HeldChanged += held => _coordinator?.SetPttHeld(held);
        _pttPoller.SetKey(_settings.PttKey);
        _pttPoller.Start();

        // Toggle-on-press, not held/released — see KeyPoller's doc comment for why this (not
        // RegisterHotKey) is what makes an unmodified single key like "L" safe to bind here
        // without also breaking that key everywhere else system-wide.
        _micMutePoller.HeldChanged += held => { if (held) Dispatcher.BeginInvoke(ToggleMicMuted); };
        _micMutePoller.SetKey(_settings.MicMuteKey);
        _micMutePoller.Start();

        _speakerMutePoller.HeldChanged += held => { if (held) Dispatcher.BeginInvoke(ToggleSpeakerMuted); };
        _speakerMutePoller.SetKey(_settings.SpeakerMuteKey);
        _speakerMutePoller.Start();

        // Auto-connect whenever we already know where to connect — either a launcher passed
        // --ip, or the user connected before and it was saved (per request: no need to click
        // Connect again every time if the server is already known).
        if (!string.IsNullOrWhiteSpace(HostTextBox.Text))
            await ConnectAsync();
    }

    private async void MainWindow_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _autoReconnectCts?.Cancel();
        _settings.ServerHost = HostTextBox.Text;
        if (int.TryParse(PortTextBox.Text, out int port)) _settings.ServerPort = port;
        _settings.ServerPassword = PasswordBox.Password;
        _settings.Save();

        _settingsWindow?.Close();
        _pttPoller.Dispose();
        _micMutePoller.Dispose();
        _speakerMutePoller.Dispose();
        if (_coordinator is not null) await _coordinator.DisposeAsync();
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        if (_settingsWindow is not null)
        {
            _settingsWindow.Activate();
            return;
        }
        if (_coordinator is null) return;

        _settingsWindow = new SettingsWindow(_settings, _coordinator, _logEntries, OnHotkeysChanged)
        {
            Owner = this,
        };
        _settingsWindow.Closed += (_, _) => _settingsWindow = null;
        _settingsWindow.Show();
    }

    /// <summary>SettingsWindow captures new key bindings into AppSettings directly, then calls
    /// this so the pollers (owned here — MainWindow is the long-lived object) pick the change up.</summary>
    private void OnHotkeysChanged()
    {
        _pttPoller.SetKey(_settings.PttKey);
        _micMutePoller.SetKey(_settings.MicMuteKey);
        _speakerMutePoller.SetKey(_settings.SpeakerMuteKey);
    }

    private async void ConnectButton_Click(object sender, RoutedEventArgs e)
    {
        _autoReconnectCts?.Cancel(); // a manual click takes priority over any in-progress auto-retry
        await ConnectAsync();
    }

    /// <summary>Retries every 5s until connected, the user disconnects/reconnects manually, or the
    /// window closes — started only after ConnectionLostUnexpectedly (never for a user-initiated
    /// disconnect). Without this, a brief server restart otherwise requires the player to notice
    /// they've gone silent and click Connect themselves.</summary>
    private async Task AutoReconnectLoopAsync()
    {
        _autoReconnectCts?.Cancel();
        var cts = new CancellationTokenSource();
        _autoReconnectCts = cts;
        var token = cts.Token;

        while (!token.IsCancellationRequested && _coordinator is not null && !_coordinator.IsConnected)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(5), token);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            if (token.IsCancellationRequested) return;
            if (string.IsNullOrWhiteSpace(HostTextBox.Text)) return;
            await ConnectAsync();
        }
    }

    private async Task ConnectAsync()
    {
        if (_coordinator is null) return;
        if (!int.TryParse(PortTextBox.Text, out int port))
        {
            Log(Loc.Get("InvalidPort"));
            return;
        }

        _connecting = true;
        ConnectButton.IsEnabled = false;
        try
        {
            // Purely cosmetic: the relay broadcasts this as this client's name in ClientJoined,
            // shown in server logs. The Arma extension no longer depends on it — it learns each
            // unit's real relay UID directly from Arma (getPlayerUID) over the pipe, not by
            // matching names (see docs/protocol-extension-legacy.md's "UID" command).
            string displayName = string.IsNullOrWhiteSpace(_cliArgs.Name) ? "TFRS" : _cliArgs.Name;
            bool ok = await _coordinator.ConnectAsync(HostTextBox.Text.Trim(), port, PasswordBox.Password, displayName);
            ConnectButton.IsEnabled = !ok;
            if (ok)
            {
                Log(Loc.Format("ConnectedTo", HostTextBox.Text, port));
                Log(string.IsNullOrEmpty(_coordinator.ServerVersion)
                    ? "Server version: unknown (older server without version reporting)"
                    : $"Server version: {_coordinator.ServerVersion}");
                if (_coordinator.ServerDebugForceAudible)
                {
                    Log("!!! SERVER DEBUG MODE: hearing everyone at full volume, ignoring the Arma addon's distance/gain — testing only.");
                    StatusText.Text += "  [DEBUG]";
                }
            }
        }
        finally
        {
            _connecting = false;
            if (_identityChangedDuringConnect)
            {
                _identityChangedDuringConnect = false;
                await ReconnectWithCurrentIdentityAsync();
            }
        }
    }

    /// <summary>The Arma extension reported the local player's real UID for the first time, or a
    /// different one than we're currently using (see VoiceSessionCoordinator.AddonUidUpdated) —
    /// it must win over whatever identity we started with (fallback or a stale CLI/persisted
    /// value). If a connect attempt is already in flight, defer until it finishes rather than
    /// racing a second one against it.</summary>
    private async void OnAddonUidUpdated()
    {
        if (_connecting) { _identityChangedDuringConnect = true; return; }
        await ReconnectWithCurrentIdentityAsync();
    }

    private async Task ReconnectWithCurrentIdentityAsync()
    {
        if (_coordinator is null) return;
        if (_coordinator.IsConnected)
        {
            await _coordinator.DisconnectAsync();
            Log(Loc.Get("Disconnected"));
        }
        if (!string.IsNullOrWhiteSpace(HostTextBox.Text))
            await ConnectAsync();
    }

    private async void DisconnectButton_Click(object sender, RoutedEventArgs e)
    {
        _autoReconnectCts?.Cancel();
        if (_coordinator is null) return;
        await _coordinator.DisconnectAsync();
        Log(Loc.Get("Disconnected"));
    }

    private void OnConnectionStateChanged(bool connected)
    {
        ConnectButton.IsEnabled = !connected;
        DisconnectButton.IsEnabled = connected;
        StatusText.Text = Loc.Get(connected ? "StatusConnected" : "StatusNotConnected");
    }

    private void MicMuteToggle_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator is not null) _coordinator.MicMuted = MicMuteToggle.IsChecked == true;
        UpdateMuteIndicators();
    }

    private void SpeakerMuteToggle_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator is not null) _coordinator.SpeakerMuted = SpeakerMuteToggle.IsChecked == true;
        UpdateMuteIndicators();
    }

    private void ToggleMicMuted()
    {
        MicMuteToggle.IsChecked = !(MicMuteToggle.IsChecked == true);
        if (_coordinator is not null) _coordinator.MicMuted = MicMuteToggle.IsChecked == true;
        UpdateMuteIndicators();
    }

    private void ToggleSpeakerMuted()
    {
        SpeakerMuteToggle.IsChecked = !(SpeakerMuteToggle.IsChecked == true);
        if (_coordinator is not null) _coordinator.SpeakerMuted = SpeakerMuteToggle.IsChecked == true;
        UpdateMuteIndicators();
    }

    /// <summary>Keeps every visual cue for "am I muted right now" in sync: toggle button color
    /// (via IsChecked, styled red in App.xaml) AND text, plus a badge next to the connection
    /// status so it's visible without looking away.</summary>
    private void UpdateMuteIndicators()
    {
        bool micMuted = MicMuteToggle.IsChecked == true;
        bool speakerMuted = SpeakerMuteToggle.IsChecked == true;

        MicMuteToggle.Content = Loc.Get(micMuted ? "MicMuted" : "MicOn");
        SpeakerMuteToggle.Content = Loc.Get(speakerMuted ? "SpeakerMuted" : "SpeakerOn");
        MicMutedBadge.Visibility = micMuted ? Visibility.Visible : Visibility.Collapsed;
        SpeakerMutedBadge.Visibility = speakerMuted ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Log(string message) => _logEntries.Insert(0, $"{DateTime.Now:HH:mm:ss}  {message}");
}
