# Neko M550D Monitor

Minimal C# / WPF desktop monitor for the Neko M550D multimeter.

## Requirements

- Windows 10/11
- .NET 8 SDK (for building)

## Run

```bash
dotnet run -c Release
```

Or use the compiled executable:

```
bin\Release\net8.0-windows\NekoM550D.Monitor.exe
```

## Single-file publish

```bash
dotnet publish -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true \
  -p:IncludeNativeLibrariesForSelfExtract=true \
  -p:DebugType=None -p:DebugSymbols=false -o publish
```

Output: `publish\NekoM550D.Monitor.exe` — a single self-contained executable (about 155 MB). It is not committed to this repository.

## Usage

1. Close Arduino IDE or any other program using the COM port.
2. Select the COM port and click **Bağlan**.
3. The Arduino resets; the first data arrives after about 3–4 seconds.
4. Use **VOLTAJ / AKIM / DİYOT / MENÜ** and **HOLD**.

## Notes

- Serial: 115200, 8N1.
- Protocol documentation: [`../../docs/SERIAL_PROTOCOL.md`](../../docs/SERIAL_PROTOCOL.md)
- Source-code comments are in Turkish; this README is in English.
