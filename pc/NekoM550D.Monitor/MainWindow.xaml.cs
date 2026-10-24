using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace NekoM550D.Monitor;

public partial class MainWindow : Window
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    private readonly MeterLink _link = new();
    private readonly DispatcherTimer _startupTimer;
    private bool _awaitingFirstStatus;

    public MainWindow()
    {
        InitializeComponent();

        _link.StatusReceived += s => Dispatcher.InvokeAsync(() => OnStatus(s));
        _link.IdentityReceived += id => Dispatcher.InvokeAsync(() => OnIdentity(id));
        _link.RawLineReceived += line => Dispatcher.InvokeAsync(() => LastLineText.Text = line);
        _link.Faulted += msg => Dispatcher.InvokeAsync(() => OnFault(msg));

        _startupTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(6) };
        _startupTimer.Tick += StartupTimer_Tick;

        SourceInitialized += (_, _) => EnableDarkTitleBar();
        Loaded += (_, _) => RefreshPorts();
        Closing += (_, _) =>
        {
            _startupTimer.Stop();
            _link.Dispose();
        };

        RefreshPorts();
        ResetUi();
    }

    private Brush B(string key) => (Brush)FindResource(key);

    private void EnableDarkTitleBar()
    {
        IntPtr hwnd = new WindowInteropHelper(this).Handle;
        int dark = 1;
        if (DwmSetWindowAttribute(hwnd, 20, ref dark, sizeof(int)) != 0)
            DwmSetWindowAttribute(hwnd, 19, ref dark, sizeof(int));
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    private void RefreshPorts()
    {
        string? selected = PortCombo.SelectedItem as string;
        string[] ports = MeterLink.GetPorts();

        PortCombo.ItemsSource = ports;

        if (ports.Length == 0)
        {
            PortCombo.SelectedIndex = -1;
            return;
        }

        int index = Array.FindIndex(ports, p => string.Equals(p, selected, StringComparison.OrdinalIgnoreCase));
        if (index < 0)
            index = Array.FindIndex(ports, p => string.Equals(p, "COM9", StringComparison.OrdinalIgnoreCase));

        PortCombo.SelectedIndex = index >= 0 ? index : 0;
    }

    private void ConnectButton_Click(object sender, RoutedEventArgs e)
    {
        if (_link.IsOpen)
        {
            Disconnect();
            return;
        }

        if (PortCombo.SelectedItem is not string port)
        {
            SetFooter("COM portu bulunamadı.", true);
            return;
        }

        try
        {
            _awaitingFirstStatus = true;

            ConnectButton.IsEnabled = false;
            PortCombo.IsEnabled = false;
            RefreshButton.IsEnabled = false;

            ConnectButton.Content = "Bağlantıyı Kes";
            ConnectButton.Background = B("Neutral");
            ConnectButton.Foreground = B("Fg");

            StatusDot.Fill = B("Warn");
            ConnectionText.Text = "Açılıyor…";
            ConnectionText.Foreground = B("Warn");

            ModeLabel.Text = "BAĞLANIYOR";
            ModeLabel.Foreground = B("Warn");
            ValueText.Text = "—";
            UnitText.Text = "";
            HintText.Text = "Arduino resetleniyor, ilk veri bekleniyor…";
            SetFooter("İlk veri bekleniyor…");

            _link.Open(port);

            ConnectButton.Background = B("Danger");
            ConnectButton.Foreground = B("OnAccent");
            ConnectButton.IsEnabled = true;

            _startupTimer.Start();
        }
        catch (Exception ex)
        {
            _startupTimer.Stop();
            _link.Close();
            ResetUi();
            HintText.Text = "Bağlantı kurulamadı.";
            SetFooter("Port açılamadı: " + ex.Message, true);
        }
    }

    private void Disconnect()
    {
        _startupTimer.Stop();
        _link.Close();
        ResetUi();
        SetFooter("Bağlantı kesildi.");
    }

    private void StartupTimer_Tick(object? sender, EventArgs e)
    {
        _startupTimer.Stop();
        if (!_awaitingFirstStatus) return;

        _awaitingFirstStatus = false;
        _link.Close();
        ResetUi();
        HintText.Text = "Cihazdan veri gelmedi.";
        SetFooter("6 saniye içinde veri gelmedi. Portu ve cihazı kontrol et.", true);
    }

    private void OnStatus(MeterStatus s)
    {
        if (_awaitingFirstStatus)
        {
            _awaitingFirstStatus = false;
            _startupTimer.Stop();

            _link.Send("#");

            EnableModeButtons(true);
            StatusDot.Fill = B("Accent");
            ConnectionText.Text = "Bağlı · " + (PortCombo.SelectedItem as string ?? "");
            ConnectionText.Foreground = B("Accent");
            SetFooter("Bağlı. Komutlar hazır.");
        }

        UpdateStatus(s);
    }

    private void OnIdentity(MeterIdentity id)
    {
        IdentityText.Text = $"{id.Device} · {id.Protocol} · {id.Firmware}";
    }

    private void UpdateStatus(MeterStatus s)
    {
        string modeName = s.Mode switch
        {
            'M' => "MENÜ",
            'V' => "VOLTAJ",
            'I' => "AKIM",
            'D' => "DİYOT",
            'C' => "ŞARJ",
            'W' => "ŞARJ UYARISI",
            _ => "BİLİNMEYEN"
        };

        string unit = s.Mode switch
        {
            'V' => "V",
            'I' => "A",
            'D' => "V",
            _ => ""
        };

        ModeLabel.Text = modeName;
        ModeLabel.Foreground = s.Mode is 'C' or 'W' ? B("Warn") : B("Accent");

        ValueText.Text = s.Value == "-" ? "—" : s.Value;
        ValueText.Foreground = s.Value == "OL" ? B("Warn") : B("Fg");
        UnitText.Text = unit;

        BatteryText.Text = s.Battery.ToString(Inv);
        BatteryBar.Value = s.Battery;
        BatteryBar.Foreground = s.Battery <= 8 ? B("Danger")
                             : s.Battery <= 15 ? B("Warn")
                             : B("Accent");

        HoldButton.Background = s.Hold ? B("Accent") : B("Neutral");
        HoldButton.Foreground = s.Hold ? B("OnAccent") : B("Fg");
        HoldStateText.Text = s.Hold ? "HOLD açık" : "HOLD kapalı";
        HoldStateText.Foreground = s.Hold ? B("Accent") : B("Muted");

        HintText.Text = s.Mode switch
        {
            'M' => "Bir mod seçin",
            'C' or 'W' => "Şarj takılı · ölçüm yapılamaz",
            _ => s.Hold ? "Değer donduruldu" : "Canlı ölçüm"
        };

        UpdateModeButtons(s.Mode);
    }

    private void UpdateModeButtons(char mode)
    {
        SetModeButton(ModeV, mode == 'V');
        SetModeButton(ModeI, mode == 'I');
        SetModeButton(ModeD, mode == 'D');
        SetModeButton(ModeM, mode == 'M');
    }

    private void SetModeButton(Button button, bool active)
    {
        button.Background = active ? B("Accent") : B("Neutral");
        button.Foreground = active ? B("OnAccent") : B("Fg");
    }

    private void EnableModeButtons(bool enabled)
    {
        ModeV.IsEnabled = enabled;
        ModeI.IsEnabled = enabled;
        ModeD.IsEnabled = enabled;
        ModeM.IsEnabled = enabled;
        HoldButton.IsEnabled = enabled;
    }

    private void ResetUi()
    {
        _awaitingFirstStatus = false;

        ConnectButton.Content = "Bağlan";
        ConnectButton.Background = B("AccentDark");
        ConnectButton.Foreground = B("OnAccent");
        ConnectButton.IsEnabled = true;

        PortCombo.IsEnabled = true;
        RefreshButton.IsEnabled = true;

        StatusDot.Fill = B("Muted");
        ConnectionText.Text = "Bağlı değil";
        ConnectionText.Foreground = B("Muted");

        ModeLabel.Text = "BAĞLI DEĞİL";
        ModeLabel.Foreground = B("Muted");
        ValueText.Text = "—";
        ValueText.Foreground = B("Fg");
        UnitText.Text = "";
        HintText.Text = "Cihazı bağlayın";

        BatteryText.Text = "—";
        BatteryBar.Value = 0;
        BatteryBar.Foreground = B("Accent");

        HoldButton.Background = B("Neutral");
        HoldButton.Foreground = B("Fg");
        HoldStateText.Text = "HOLD kapalı";
        HoldStateText.Foreground = B("Muted");

        IdentityText.Text = "PROTOKOL — · FW —";
        LastLineText.Text = "—";

        UpdateModeButtons('\0');
        EnableModeButtons(false);
        SetFooter("Hazır.");
    }

    private void OnFault(string message)
    {
        if (!_link.IsOpen) return;

        _startupTimer.Stop();
        _link.Close();
        ResetUi();
        HintText.Text = "Bağlantı koptu.";
        SetFooter(message, true);
    }

    private void SetFooter(string text, bool warn = false)
    {
        FooterText.Text = text;
        FooterText.Foreground = warn ? B("Warn") : B("Muted");
    }

    private void ModeButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_link.IsOpen) return;
        if (sender is Button { Tag: string command }) _link.Send(command);
    }

    private void HoldButton_Click(object sender, RoutedEventArgs e)
    {
        if (_link.IsOpen) _link.Send("H");
    }

    private void RefreshButton_Click(object sender, RoutedEventArgs e)
    {
        if (!_link.IsOpen) RefreshPorts();
    }
}
