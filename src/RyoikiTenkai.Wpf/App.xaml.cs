using System.Windows;
using System.Windows.Threading;

namespace RyoikiTenkai.Wpf;

public partial class App : Application
{
    private readonly string _logPath = System.IO.Path.Combine(AppContext.BaseDirectory, "ryoikitenkai.log");

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;
        TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        WriteFatalLog("DispatcherUnhandledException", e.Exception);
        e.Handled = true;
        MessageBox.Show(e.Exception.ToString(), "RyoikiTenkai error", MessageBoxButton.OK, MessageBoxImage.Error);
    }

    private void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        WriteFatalLog("UnhandledException", e.ExceptionObject as Exception);
    }

    private void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
    {
        WriteFatalLog("UnobservedTaskException", e.Exception);
        e.SetObserved();
    }

    private void WriteFatalLog(string kind, Exception? exception)
    {
        try
        {
            System.IO.File.AppendAllText(_logPath, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}  {kind}: {exception}{Environment.NewLine}");
        }
        catch
        {
        }
    }
}
