using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace Tfrs.VoiceClient.Hotkeys;

/// <summary>
/// Global single-press hotkeys (mic-mute / speaker-mute toggles) via the standard Win32
/// RegisterHotKey API — the same mechanism ordinary Windows apps use for global shortcuts, works
/// while Arma 3 has focus, and doesn't read arbitrary keystrokes the way a keyboard hook would.
/// </summary>
internal sealed class GlobalHotkeyManager : IDisposable
{
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    private const int WM_HOTKEY = 0x0312;

    private readonly HwndSource _hwndSource;
    private readonly Dictionary<int, Action> _handlers = new();
    private int _nextId = 0xC000; // arbitrary range unlikely to collide with other apps' IDs

    public GlobalHotkeyManager(IntPtr windowHandle)
    {
        _hwndSource = HwndSource.FromHwnd(windowHandle) ?? throw new InvalidOperationException("Window handle not yet created.");
        _hwndSource.AddHook(WndProc);
    }

    /// <summary>Registers a hotkey with no modifiers (matches the "auf eine Tastaturtaste binden"
    /// requirement — plain keyboard keys, no Ctrl/Alt/Shift combos required). Returns a token to
    /// pass to <see cref="Unregister"/>.</summary>
    public int Register(int vKey, Action onPressed)
    {
        int id = _nextId++;
        if (!RegisterHotKey(_hwndSource.Handle, id, 0, (uint)vKey))
            throw new InvalidOperationException($"Konnte Hotkey (VK={vKey}) nicht registrieren — evtl. bereits von einer anderen Anwendung belegt.");
        _handlers[id] = onPressed;
        return id;
    }

    public void Unregister(int id)
    {
        if (!_handlers.Remove(id)) return;
        UnregisterHotKey(_hwndSource.Handle, id);
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_HOTKEY && _handlers.TryGetValue(wParam.ToInt32(), out var action))
        {
            action();
            handled = true;
        }
        return IntPtr.Zero;
    }

    public void Dispose()
    {
        foreach (int id in _handlers.Keys.ToArray())
            UnregisterHotKey(_hwndSource.Handle, id);
        _handlers.Clear();
        _hwndSource.RemoveHook(WndProc);
    }
}
