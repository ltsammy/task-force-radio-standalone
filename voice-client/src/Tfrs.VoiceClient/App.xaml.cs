using System.IO;
using System.Windows;
using System.Windows.Threading;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // A single unhandled exception on any background thread (audio callback, network
        // receive loop, bridge pipe thread, ...) kills the whole process by CLR design, and it
        // can look like the window just froze in the moment before it disappears. This can't be
        // prevented after the fact, but logging first turns a silent, unreproducible crash into
        // something we can actually diagnose. UI-thread exceptions we CAN keep the app alive for.
        DispatcherUnhandledException += (_, args) =>
        {
            LogCrash("UI thread", args.Exception);
            args.Handled = true;
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
            LogCrash("background thread (fatal)", args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            LogCrash("unobserved task", args.Exception);
            args.SetObserved();
        };

        var settings = AppSettings.Load();
        var cliArgs = CommandLineArgs.Parse(e.Args);

        var window = new MainWindow(settings, cliArgs);
        MainWindow = window;
        window.Show();
    }

    private static void LogCrash(string source, Exception? ex)
    {
        try
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Tfrs", "VoiceClient");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "crash.log"),
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {source}: {ex}\n\n");
        }
        catch
        {
            // best-effort — must never throw from a crash handler
        }
    }
}
