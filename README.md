# Saab Dash

iPhone-ersättare för den runda CAN-displayen i Saab 9-3 (T8). En dongel i OBD-uttaget läser bilen, lagrar appen och strömmar värden till Safari över telefonens hotspot.

```
Bil ─CAN (P-bus 500k)─ ESP32-dongel (WiCAN-OBD) ══ Wi-Fi (hotspot) ══ iPhone, Safari
```

## Innehåll
| Sökväg | Vad |
|---|---|
| `PROTOCOL.md` | Protokollet mellan dongel och app: WebSocket, paketformat, kommandon, uppdatering |
| `web/app.html` | Appens källa |
| `web/build.sh` | Bygger `index.html`, `docs/index.html`, ikoner och `version.json` |
| `index.html`, `docs/` | Byggd app som GitHub Pages publicerar (ändra inte för hand) |
| `tools/mock_dongle.py` | Låtsasdongel för test utan hårdvara (bara Pythons standardbibliotek) |
| `firmware/` | (kommer) ESP32-firmware för dongeln |

## Använda appen
- **I bilen:** slå på hotspot, öppna `http://saab-dash.local` i Safari och lägg till på hemskärmen. Appen ansluter själv.
- **Utan dongel:** GitHub Pages-adressen visar demoläge.
- Svep eller tryck för att byta sida. **Night** släcker allt utom det viktigaste. **Logg** visar paket, rådata, dongelns konsol och uppdatering.

## Uppdatera
Öppna **Logg** i appen när den är öppnad från dongeln:
- **Uppdatera appen** hämtar senaste `index.html` från GitHub Pages och skickar den till dongeln.
- **Uppdatera firmware** hämtar senaste firmware när en sådan finns publicerad.
- **Ladda upp fil…** skickar en `.html` (app) eller `.bin` (firmware) från iPhonens Filer-app.

## Testa utan dongel
```
python3 tools/mock_dongle.py
```
Öppna `http://localhost:8080`, eller adressen som skrivs ut, från en iPhone på samma Wi-Fi.

## Utveckla
Ändra `web/app.html`, kör `web/build.sh`, committa allt.
