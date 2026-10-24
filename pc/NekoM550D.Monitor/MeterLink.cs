using System.Globalization;
using System.IO.Ports;
using System.Text;

namespace NekoM550D.Monitor;

public sealed record MeterStatus(char Mode, string Value, int Battery, bool Hold);

public sealed record MeterIdentity(string Device, string Protocol, string Firmware);

/// <summary>
/// Neko M550D seri protokolü: 115200 8N1, satırlar '\n' ile biter.
/// Gelen: MOD,VAL,PIL,HOLD  ·  Giden: V/I/D/M/H/?
/// </summary>
public sealed class MeterLink : IDisposable
{
    private readonly object _writeLock = new();
    private SerialPort? _port;
    private CancellationTokenSource? _cts;
    private Task? _reader;
    private int _closing;

    public event Action<MeterStatus>? StatusReceived;
    public event Action<MeterIdentity>? IdentityReceived;
    public event Action<string>? RawLineReceived;
    public event Action<string>? Faulted;

    public bool IsOpen => _port?.IsOpen == true;

    public static string[] GetPorts() =>
        SerialPort.GetPortNames()
            .OrderBy(p => p, StringComparer.OrdinalIgnoreCase)
            .ToArray();

    public void Open(string portName)
    {
        Close();

        var port = new SerialPort(portName, 115200, Parity.None, 8, StopBits.One)
        {
            NewLine = "\n",
            ReadTimeout = 250,
            WriteTimeout = 1000,
            DtrEnable = true,
            RtsEnable = true,
            Handshake = Handshake.None,
            Encoding = Encoding.ASCII
        };

        port.Open();
        _port = port;
        _closing = 0;
        _cts = new CancellationTokenSource();
        _reader = Task.Run(() => ReadLoop(port, _cts.Token));
    }

    public void Send(string command)
    {
        var port = _port;
        if (port is null || !port.IsOpen) return;

        try
        {
            lock (_writeLock) port.Write(command + "\n");
        }
        catch (Exception ex)
        {
            Faulted?.Invoke("Komut gönderilemedi: " + ex.Message);
        }
    }

    private void ReadLoop(SerialPort port, CancellationToken ct)
    {
        var buffer = new StringBuilder();

        try
        {
            while (!ct.IsCancellationRequested && port.IsOpen)
            {
                string chunk;
                try { chunk = port.ReadExisting(); }
                catch { break; }

                if (chunk.Length == 0)
                {
                    Thread.Sleep(20);
                    continue;
                }

                foreach (char c in chunk)
                {
                    if (c == '\n')
                    {
                        string line = buffer.ToString().TrimEnd('\r');
                        buffer.Clear();
                        if (line.Length > 0) HandleLine(line);
                    }
                    else if (c != '\r')
                    {
                        if (buffer.Length < 256) buffer.Append(c);
                        else buffer.Clear();
                    }
                }
            }
        }
        catch
        {
            // Bağlantı koptu; aşağıdaki finally bildirir.
        }
        finally
        {
            if (Volatile.Read(ref _closing) == 0)
                Faulted?.Invoke("Seri bağlantı koptu.");
        }
    }

    private void HandleLine(string line)
    {
        RawLineReceived?.Invoke(line);

        if (line[0] == '#')
        {
            var id = line[1..].Split(',');
            if (id.Length >= 3)
                IdentityReceived?.Invoke(new MeterIdentity(id[0], id[1], id[2]));
            return;
        }

        var f = line.Split(',');
        if (f.Length != 4) return;

        char mode = f[0].Length > 0 ? char.ToUpperInvariant(f[0][0]) : '?';
        if (!int.TryParse(f[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out int battery))
            return;

        StatusReceived?.Invoke(new MeterStatus(
            mode,
            f[1],
            Math.Clamp(battery, 0, 100),
            f[3].Trim() == "1"));
    }

    public void Close()
    {
        Interlocked.Exchange(ref _closing, 1);

        var cts = _cts;
        _cts = null;
        if (cts is not null)
        {
            try { cts.Cancel(); } catch { }
            cts.Dispose();
        }

        var port = _port;
        _port = null;
        if (port is not null)
        {
            try { if (port.IsOpen) port.Close(); } catch { }
            try { port.Dispose(); } catch { }
        }

        try { _reader?.Wait(400); } catch { }
        _reader = null;
    }

    public void Dispose() => Close();
}
