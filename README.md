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
| `firmware/` | ESP32-C3-firmware för dongeln (ESP-IDF). Byggs av GitHub Actions. CAN är inte inkopplat än. |

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

## Första start av dongeln
Utan sparad hotspot, eller om ingen hittas på 30 s, öppnar dongeln ett eget Wi-Fi: **Saab-Dash**, lösenord `saab9-3t8`. Anslut iPhonen till det, öppna `http://192.168.4.1`, gå till **Logg** och skriv i konsolen:
```
wifi Toms iPhone|hotspotlösenord
```
Dongeln sparar det och ansluter sedan själv till hotspoten.

## Utveckla
Ändra `web/app.html`, kör `web/build.sh`, committa allt.
