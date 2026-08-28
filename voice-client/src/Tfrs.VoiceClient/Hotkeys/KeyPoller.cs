using System.Runtime.InteropServices;

namespace Tfrs.VoiceClient.Hotkeys;

/// <summary>
/// Polls the held/released state of one configured key via GetAsyncKeyState, independent of
/// window focus (Arma 3 has focus while the player pushes to talk / hits mute). Used for every
/// bindable action (PTT, mic mute, speaker mute) — deliberately NOT:
///
/// - A low-level keyboard hook (WH_KEYBOARD_LL): hooks read every keystroke system-wide and are
///   exactly the pattern AV/anti-cheat heuristics flag as keylogger-like; polling a single
///   configured VK code is the same approach Mumble/TeamSpeak use for PTT and reads as ordinary,
///   narrowly-scoped input polling.
/// - RegisterHotKey: it can't express "currently held" (Windows only fires it once per press,
///   with its own repeat-throttling and no reliable release event), which PTT needs outright. It
///   also has a sharper problem for a single unmodified key like a bare "L": Windows delivers
///   that keystroke to the registering app's hotkey handler *instead of* the normally focused
///   window, so the bound key silently stops working everywhere else system-wide (including
///   normal typing) for as long as the app runs — this is what happened when mic/speaker mute
///   used GlobalHotkeyManager. Polling has no such effect: it only reads state, it never consumes
///   the keystroke, so the key keeps working normally everywhere else too.
/// </summary>
internal sealed class KeyPoller : IDisposable
{
    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    private const int PollIntervalMs = 15;
    private readonly System.Threading.Timer _timer;
    private int _vKey;
    private bool _wasHeld;

    public event Action<bool>? HeldChanged;

    public KeyPoller()
    {
        _timer = new System.Threading.Timer(_ => Poll(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public void SetKey(int vKey)
    {
        _vKey = vKey;
        _wasHeld = false;
    }

    public void Start() => _timer.Change(0, PollIntervalMs);
    public void Stop() => _timer.Change(Timeout.Infinite, Timeout.Infinite);

    private void Poll()
    {
        int vKey = _vKey;
        if (vKey == 0) return;

        bool held = (GetAsyncKeyState(vKey) & 0x8000) != 0;
        if (held == _wasHeld) return;
        _wasHeld = held;
        HeldChanged?.Invoke(held);
    }

    public void Dispose() => _timer.Dispose();
}
