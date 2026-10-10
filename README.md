# Saab Dash

iPhone-ersättare för den runda CAN-displayen i Saab 9-3 (T8). En BLE-dongel i OBD-uttaget läser bilen och skickar färdiga värden till en webbapp som körs i Bluefy på telefonen.

```
Bil ─CAN (P-bus 500k)─ ESP32-dongel (WiCAN-OBD) ─BLE─ iPhone (Bluefy)
```

## Innehåll
| Sökväg | Vad |
|---|---|
| `PROTOCOL.md` | BLE-protokollet mellan dongel och app (GATT-UUID:er, paketformat, kommandon) |
| `web/app.html` | Appens källa |
| `web/build.sh` | Bygger `docs/index.html` från källan |
| `index.html`, `docs/index.html` | Fristående sida som GitHub Pages publicerar (byggs, ändra inte för hand) |
| `firmware/` | (kommer) ESP32-firmware för dongeln |

## Använda appen
1. Öppna Pages-adressen i appen **Bluefy** på iPhone (Safari saknar Web Bluetooth).
2. Tryck **Anslut** och välj `SAAB-…`. Utan dongel visar **Demo** simulerad data.
3. Svep eller tryck för att byta sida. **Night** släcker allt utom det viktigaste. **Logg** visar paket, rådata och dongelns konsol.

## Utveckla
Ändra `web/app.html`, kör `web/build.sh`, committa båda.
