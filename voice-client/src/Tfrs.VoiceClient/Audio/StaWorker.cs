using System.Collections.Concurrent;

namespace Tfrs.VoiceClient.Audio;

/// <summary>
/// Runs actions on one dedicated STA background thread. WASAPI device activation
/// (<c>IMMDevice.Activate</c>, used by NAudio's <c>WasapiCapture</c>/<c>WasapiOut</c>
/// constructors) is a COM call that needs an STA apartment — the same requirement the WPF UI
/// thread happens to satisfy, which is why running it inline used to "work" (while blocking the
/// UI). Moving it to a plain <c>Task.Run</c> instead breaks it outright: the default thread pool
/// runs MTA, and activating there throws <see cref="InvalidCastException"/> from the COM
/// marshaling stub. This gives WASAPI setup/teardown a correctly-configured thread that isn't
/// the UI thread either, so it can't freeze the window.
/// </summary>
internal sealed class StaWorker : IDisposable
{
    private readonly BlockingCollection<Action> _queue = new();
    private readonly Thread _thread;

    public StaWorker()
    {
        _thread = new Thread(RunLoop) { IsBackground = true, Name = "TFRS-Audio-STA" };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    private void RunLoop()
    {
        foreach (var action in _queue.GetConsumingEnumerable())
        {
            action();
        }
    }

    public Task InvokeAsync(Action action)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _queue.Add(() =>
        {
            try
            {
                action();
                tcs.SetResult();
            }
            catch (Exception ex)
            {
                tcs.SetException(ex);
            }
        });
        return tcs.Task;
    }

    public void Dispose() => _queue.CompleteAdding();
}
