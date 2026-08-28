using System.Windows;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var settings = AppSettings.Load();
        var cliArgs = CommandLineArgs.Parse(e.Args);

        var window = new MainWindow(settings, cliArgs);
        MainWindow = window;
        window.Show();
    }
}
