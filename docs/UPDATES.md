# SRTune — instalace a aktualizace

SRTune používá pro Windows stejný základní model jako Airlet PDF: Inno Setup instalátor, veřejné GitHub Releases a ručně spuštěnou integrovanou aktualizaci. Aktualizace se nikdy neinstaluje z GitHub Actions artefaktu.

## Instalace

Instalátor používá stálé Inno Setup `AppId` a instaluje SRTune pro aktuálního uživatele do:

`%LOCALAPPDATA%\Programs\SRTune`

Novější instalátor se instaluje přes stávající instalaci do stejné složky. Uživatelská pracovní data a projekty nejsou ukládány do instalačního adresáře.

Portable ZIP zůstává samostatná varianta. Portable verze se automaticky nepřevádí na instalovanou aplikaci a integrovaný updater ji neaktualizuje.

## Ruční aktualizace

V aplikaci zvolte **Nápověda → Zkontrolovat aktualizace…**.

SRTune zkopíruje malý nativní `SRTune.Update.exe` do dočasné složky a spustí jej mimo instalační adresář. Helper:

1. načte aktuální instalovanou verzi z Inno Setup registrace,
2. ručně zkontroluje poslední stabilní veřejný GitHub Release,
3. odmítne draft, prerelease, downgrade a neplatné SemVer,
4. stáhne `srtune-update.json`,
5. ověří přesný název instalátoru, velikost a SHA-256,
6. po potvrzení stáhne instalační EXE do dočasné složky,
7. ověří jeho velikost a SHA-256,
8. po druhém potvrzení požádá SRTune o ukončení,
9. před instalací znovu ověří SHA-256,
10. spustí Inno Setup velmi tiše do stejné instalační složky,
11. po úspěchu znovu spustí SRTune.

Pokud SRTune při ukončování zjistí neuložené změny, standardní potvrzení uložení zůstává aktivní. Zrušení zavření znamená, že se aktualizace neprovede.

Aktualizátor používá pouze HTTPS a GitHub hosty potřebné pro API a release assets. Instalátor má limit 700 MiB. SHA-256 chrání integritu vůči manifestu získanému z veřejného GitHub Release; projekt zatím nepoužívá Authenticode podpis.

## Publikace nové stabilní verze

Workflow **SRTune Windows Release** se spouští pouze ručně přes GitHub Actions.

- Zadejte verzi ve formátu `X.Y.Z`.
- Bez volby `publish_release` workflow pouze sestaví a nahraje testovací artefakty.
- S volbou `publish_release` musí existovat `docs/releases/X.Y.Z.md`.
- Workflow vytvoří Inno instalátor, portable ZIP, `SHA256SUMS.txt` a `srtune-update.json`.
- Release se nejprve vytvoří jako draft a zveřejní se až po úspěšném nahrání všech assetů.
- Existující publikovaný tag se nikdy nepřepisuje.

Aktualizační manifest má schéma:

```json
{
  "schema": 1,
  "version": "X.Y.Z",
  "installer": {
    "name": "SRTune-X.Y.Z-Setup-x64.exe",
    "size": 123456789,
    "sha256": "..."
  }
}
```

## První přechod

Veřejná verze SRTune 1.0.0 ještě integrovaný updater neobsahuje. První verzi, která tento mechanismus obsahuje, je proto nutné jednorázově nainstalovat běžným instalátorem přes 1.0.0. Od následující stabilní verze už lze další aktualizace provádět přímo z aplikace.

## Vývojový kanál

PhotoLibrarian navíc podporuje volitelný development/prerelease kanál. SRTune jej zatím nezapíná; stabilní updater přijímá pouze standardní veřejné stabilní release. Infrastruktura je oddělená tak, aby šlo vývojový kanál později doplnit bez změny instalačního modelu.
