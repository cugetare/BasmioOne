# Storybox SD Prep (WPF, .NET 8)

Aplicatie desktop pentru Windows care pregateste cardul SD pentru Storybox.

## Functionalitati implementate
- Import MP3: fiecare fisier devine o poveste cu 1 capitol.
- Citire metadata ID3:
  - titlu din `TIT2` (fallback: nume fisier)
  - track din `TRCK` (fallback ID3v1)
- Comasare povesti (selectie multipla + click dreapta):
  - creeaza o poveste noua
  - sterge povestile selectate din lista
  - fiecare element devine capitol
- Sortare capitole:
  - `Track` ID3 crescator
  - capitole fara track la final, alfabetic
- Editare detalii poveste:
  - titlu poveste
  - titlu capitole
  - cover image
- Cover image:
  - `Paste` din clipboard (Ctrl+V)
  - conversie automata la PNG 400x400
  - export cu numele `cover.png`
- Prepare SD Card:
  - selectare drive
  - optional formatare FAT32 prin utilitar extern
  - creeaza pe SD cate un folder per poveste
  - copiaza capitolele ca `01 - Titlu.mp3`, `02 - Titlu.mp3`, etc.

## Structura exportata pe SD
Exemplu pentru povestea `Alba ca Zapada`:

```
E:\Alba ca Zapada\
  01 - Alba ca Zapada.mp3
  cover.png
```

## Utilitar FAT32 extern
Aplicatia asteapta implicit utilitarul la:

```
<folder aplicatie>\tools\fat32format.exe
```

In fereastra `Prepare SD Card` poti alege manual alta locatie pentru utilitar.

## Build / run
Proiectul este WPF pe `.NET 8`:

```
dotnet restore
dotnet build -c Release
dotnet publish -c Release -r win-x64 --self-contained false
```

## Observatie
In mediul curent unde a fost generat codul exista runtime, dar nu exista SDK .NET instalat, deci build-ul local nu a putut fi executat aici.