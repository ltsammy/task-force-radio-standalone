using System.Runtime.InteropServices;

namespace Tfrs.VoiceClient.Hotkeys;

/// <summary>
/// Polls the held/released state of a configured key via GetAsyncKeyState, independent of window
/// focus (Arma 3 has focus while the player pushes to talk). Deliberately NOT a low-level keyboard
/// hook (WH_KEYBOARD_LL) — hooks read every keystroke system-wide and are exactly the pattern
/// AV/anti-cheat heuristics flag as keylogger-like; polling a single configured VK code is the same
/// approach Mumble/TeamSpeak use for PTT and reads as ordinary, narrowly-scoped input polling.
/// RegisterHotKey (see GlobalHotkeyManager) can't express "currently held" — Windows only fires it
/// once per press with its own repeat-throttling and no reliable release event — so PTT needs this.
/// </summary>
internal sealed class PttKeyPoller : IDisposable
{
    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    private const int PollIntervalMs = 15;
    private readonly System.Threading.Timer _timer;
    private int _vKey;
    private bool _wasHeld;

    public event Action<bool>? HeldChanged;

    public PttKeyPoller()
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
