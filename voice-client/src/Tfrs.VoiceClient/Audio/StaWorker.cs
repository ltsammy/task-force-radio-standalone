using System.Windows.Threading;

namespace Tfrs.VoiceClient.Audio;

/// <summary>
/// Runs actions on one dedicated STA background thread with a real Windows message pump.
///
/// WASAPI device activation (<c>IMMDevice.Activate</c>, used by NAudio's
/// <c>WasapiCapture</c>/<c>WasapiOut</c> constructors) is a COM call that needs an STA apartment
/// — the same requirement the WPF UI thread happens to satisfy, which is why running it inline
/// used to "work" (while blocking the UI). A plain <c>Task.Run</c> instead breaks it outright:
/// the default thread pool runs MTA, and activating there throws <see cref="InvalidCastException"/>
/// from the COM marshaling stub.
///
/// A bare STA thread isn't enough either: STA apartments are expected to pump messages for the
/// lifetime of any COM object created on them, not just for the moment of activation — the
/// WasapiCapture/WasapiOut objects live for as long as the connection does. A thread that just
/// loops over a work queue with no message pump can activate the device once and then hang (or
/// leave the whole app looking frozen) the first time WASAPI needs to call back into that
/// apartment. <see cref="Dispatcher"/> is WPF's own answer to exactly this problem — it's a
/// message pump plus a thread-safe invoke queue — so this thread runs one instead of hand-rolling
/// a queue consumer.
/// </summary>
internal sealed class StaWorker : IDisposable
{
    private readonly Thread _thread;
    private readonly TaskCompletionSource<Dispatcher> _dispatcherReady = new();

    public StaWorker()
    {
        _thread = new Thread(() =>
        {
            _dispatcherReady.SetResult(Dispatcher.CurrentDispatcher);
            Dispatcher.Run();
        })
        { IsBackground = true, Name = "TFRS-Audio-STA" };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    public async Task InvokeAsync(Action action)
    {
        var dispatcher = await _dispatcherReady.Task;
        await dispatcher.InvokeAsync(action).Task;
    }

    public void Dispose()
    {
        if (_dispatcherReady.Task.IsCompletedSuccessfully)
            _dispatcherReady.Task.Result.InvokeShutdown();
    }
}
