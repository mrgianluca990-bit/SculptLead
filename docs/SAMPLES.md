# Banco acustico di Sculpt Lead

Il banco incluso usa registrazioni reali di **VS Chamber Orchestra: Community
Edition (VSCO 2 CE)**, di Versilian Studios. Non richiede un sampler esterno,
SoundFont, download al primo avvio o Python durante l'uso del plugin.

## Contenuto

| Strumento | Famiglia | Note originali MIDI | Dinamiche reali | File preparati |
|---|---|---|---|---:|
| Clarinetto | Reed, registro basso/medio | 50, 53, 58, 62, 65, 70, 74, 77, 82, 86, 89 | 3 per nota | 66 |
| Flauto senza vibrato | Reed, registro medio/alto | 60, 64, 69, 72, 76, 81, 84, 88, 93, 96 | 2; una sola a MIDI 96 | 34 |
| Trombone tenore | Brass, registro basso/medio | 34, 37, 39, 41, 46, 50, 53, 60, 61, 63, 65 | 3; 2 a MIDI 34 e 37 | 62 |
| Tromba aperta | Brass, registro medio/alto | 53, 57, 60, 63, 67, 70, 74, 77, 81, 84 | 2 per nota | 40 |

Sono **202 file**, corrispondenti a **103 combinazioni di nota e dinamica**.
Il contenuto WAV pesa circa **33,64 MB / 32,08 MiB**, è mono PCM a 16 bit e
48.000 Hz. I file sono inclusi nel binario del plugin/applicazione: non servono
cartelle esterne da installare o cercare. La conversione in float del banco
richiede all'incirca il doppio dei byte audio PCM, oltre alle risorse incorporate
e alle strutture del programma; la dimensione del banco non è una misura di CPU.

Le note indicate sono i centri reali dei campioni, non 202 note tutte registrate
separatamente. Tra questi centri il motore riproduce il campione vicino con la
trasposizione necessaria. Fuori dal registro originale la trasposizione resta
possibile, ma non aggiunge registrazioni reali di quelle note.

Con la convenzione **C4 = MIDI 60**, il clarinetto va da D3 a F6, il flauto da
C4 a C7, il trombone da Bb1 a F4 e la tromba da F3 a C6. I nomi di file VSCO
usano un'altra convenzione d'ottava: il riferimento di importazione è sempre il
`pitch_keycenter` dell'SFZ originale, non il testo del nome del WAV.

### Incroci consigliati

- **Reed:** passaggio progressivo clarinetto → flauto tra MIDI 67 e 79.
- **Brass:** passaggio progressivo trombone → tromba tra MIDI 58 e 67.

Il registro basso del flauto rimane disponibile, ma il crossover favorisce il
clarinetto in quella zona. Il trombone smette di predominare vicino alla parte
alta della sua estensione registrata.

## Che cosa significa il round robin

Le registrazioni sustain VSCO selezionate hanno **una sola esecuzione per nota e
dinamica**. Le articolazioni staccato forniscono due attacchi realmente registrati
(`rr1` e `rr2`). Per ottenere note lead che possano essere tenute:

1. si conserva l'attacco della registrazione staccato, con il suo transiente e la
   sua articolazione;
2. si allinea intonazione, livello e fase al corpo della registrazione sustain;
3. una dissolvenza di 80 ms passa al sustain;
4. il corpo sustain prosegue in un loop scelto nella registrazione reale.

Quindi il round robin varia **l'attacco registrato** e condivide il sustain
tra le due varianti. Non sono due esecuzioni complete e indipendenti della nota
lunga. Non sono registrazioni di intervalli legato: il legato è gestito dal
motore monofonico.

Eccezioni e adattamenti espliciti:

- A MIDI **60 e 64 il flauto non ha staccati registrati** in questa libreria.
  Le due dinamiche di ciascuna nota mantengono il proprio attacco sustain
  originale e un solo round robin. Non sono state fabbricate varianti casuali.
- Il trombone ha una griglia di staccati meno fitta del sustain. In alcune note
  viene usato l'attacco più vicino, trasposto **al massimo di tre semitoni**;
  il corpo resta la registrazione sustain della nota corretta.
- Le dinamiche degli staccati e quelle dei sustain non hanno sempre lo stesso
  numero. L'attacco viene abbinato per posizione dinamica (piano/medio/forte),
  senza dichiarare layer aggiuntivi. A MIDI 86 il clarinetto ha due dinamiche
  staccato per tre dinamiche sustain.

Ogni regione del manifest dichiara i file originali, la dinamica originale,
l'RR dell'attacco, l'eventuale trasposizione, i punti di splice e gli hash.

## Preparazione del suono

Le sorgenti sono state verificate sia con l'hash Git del blob originale sia con
SHA-256. L'importatore ripete la verifica SHA-256 prima di leggere ogni file.
La ricetta è riproducibile e non usa modelli generativi o librerie proprietarie.

- Somma mono dei canali stereo; viene selezionato un singolo canale soltanto
  nell'eventualità di una cancellazione di fase molto forte. Il criterio e la
  scelta effettiva sono registrati nel manifest.
- Ricampionamento polifase a 48 kHz.
- Filtro passa-alto a 20 Hz per eliminare DC e infrabasse. Non è applicato un
  filtro passa-basso che cancelli il soffio e le armoniche naturali.
- Rimozione del silenzio iniziale superfluo, mantenendo un piccolo margine prima
  dell'attacco. Una rampa di 1 ms evita un bordo digitale.
- Quando il preroll molto debole supera 40 ms prima di raggiungere il 10% del
  livello RMS del sustain, viene accorciato mantenendo 12 ms prima di quella
  soglia. Si usa una finestra RMS di 5 ms. Il transiente registrato rimane;
  un attacco naturale già udibile e graduale non viene trasformato in un click.
- Stima armonica dell'intonazione e piccola correzione statica quando la stima
  è sufficientemente attendibile. Il movimento naturale di intonazione durante
  la registrazione rimane nel campione; non è un pitch flattening.
- Allineamento di fase e livello nell'incrocio attacco/sustain. La correlazione
  misurata controlla una compensazione di energia durante gli 80 ms di splice:
  gli attacchi meno correlati non producono un piccolo vuoto di volume, mentre
  due forme d'onda già correlate non ricevono il boost di un crossfade equal power.
- Ricerca di un loop con preroll correlato e livello simile. Ogni loop dura
  almeno 720 ms, conservando una porzione di esecuzione reale.
- Dither TPDF deterministico prima della conversione PCM16.

Le dinamiche **non sono livellate tutte alla stessa ampiezza**. Gli obiettivi RMS
del sustain sono circa −23,10 dBFS per il piano, −20,09 dBFS per la dinamica
intermedia e −17,08 dBFS per il forte. Due layer usano gli estremi; un unico
layer usa il livello intermedio. Il timbro originale dei diversi layer rimane
distinto, e la velocity del motore continua a controllare l'esecuzione.

La coppia RR di una nota/dinamica riceve lo stesso eventuale abbassamento di
sicurezza per i picchi. `gainDb` nel manifest è **0**, perché questa calibrazione
è già incorporata nei WAV. Non applicare di nuovo una normalizzazione separata
a ciascun file in fase di caricamento.

## Contratto di riproduzione dei loop

Gli indici sono frame mono a 48 kHz. `loopStart` è incluso e `loopEnd` è
**esclusivo**. Il periodo è `loopEnd - loopStart`.

Negli ultimi N frame prima di `loopEnd`, il motore miscela la coda con i N frame
**immediatamente precedenti** `loopStart`, poi riparte esattamente da
`loopStart`. N è il minimo tra 20 ms, un quarto della durata del loop e
`loopStart`. La curva di crossfade è smoothstep in ampiezza lineare:
`w = t*t*(3 - 2*t)`; `out = tail*(1-w) + preroll*w`.

Non usare un secondo crossfade verso i frame **successivi** a `loopStart` e non
saltare a `loopStart + N`: questa sarebbe una diversa convenzione di loop.
Il WAV contiene anche alcuni frame oltre `loopEnd` per un'interpolazione sicura.
I due attacchi di una stessa nota/dinamica hanno lo stesso corpo originale, anche
quando la posizione assoluta del loop differisce a causa dell'allineamento.

## Rigenerazione e verifica

Richiede Python 3.10+, NumPy e SciPy solo sul computer che prepara il banco:

```sh
python -m pip install numpy scipy
python tools/import_factory_samples.py --cache /percorso/cache-vsco
python tools/import_factory_samples.py --verify
```

Per lavorare con tutte le sorgenti già scaricate:

```sh
python tools/import_factory_samples.py --cache /percorso/cache-vsco --offline
```

Il primo comando può scaricare circa **241 MB** di WAV sorgenti e documenti
CC0. Il cache dei sorgenti non deve essere distribuito né incorporato nel plugin;
per compilare sono sufficienti i WAV già preparati in `Samples/`.

`--verify` controlla hash dei WAV preparati, formato PCM, unicità globale dei
basename, limiti e durata dei loop, assenza di clipping digitale e continuità
del wrap con il crossfade specificato. La verifica del banco di questa versione
ha rilevato un residuo massimo del wrap di circa 1,8 × 10⁻⁷ in ampiezza float,
oltre alla normale variazione tra due frame adiacenti della forma d'onda.

I numeri precisi e gli hash sono in `Samples/manifest.json`; il report dei loop
è in `Samples/build-report.json`. `tools/factory_sources.json` contiene le URL
immutabili, SHA-256, hash Git, mappature e abbinamenti di ogni sorgente.

### Reattività e limite del flauto basso

Il controllo degli onset distingue il preroll quasi silenzioso dalla salita
naturale della nota. Dopo il taglio del preroll debole, il banco supera il 10%
del livello RMS del sustain entro 40 ms. Molti attacchi partono molto prima.
Questo dato riguarda i WAV alla loro nota originale, non la latenza della
scheda audio, del buffer host o dell'inviluppo del motore.

Il flauto basso senza staccati mantiene l'attacco sustain originale: in un caso
la crescita fino a metà livello RMS richiede circa 197 ms. Il suono è già
udibile prima, ma quella salita morbida è parte della registrazione. È un limite
reale della libreria scelta; sotto il crossover Reed prevale il clarinetto,
che evita di affidare il registro basso a quel flauto lento.

Ambiente usato per preparare questa versione: Python 3.12.14, NumPy 2.3.5,
SciPy 1.17.0. L'esecuzione del plugin non richiede questi pacchetti.

## Origine e licenza

- Progetto: <https://github.com/sgossner/VSCO-2-CE>
- Sito ufficiale: <https://versilian-studios.com/vsco-community/>
- Commit dei WAV: `440300901dfe9275fd84e0b7763af1f8443ae62e`.
- Mappature originali SFZ: release `1.1.0`, copiate in `ThirdParty/VSCO/`
  e verificate con SHA-256 nel file delle sorgenti.
- Licenza delle registrazioni: **CC0 1.0 Universal**, testo completo originale
  in `ThirdParty/VSCO/LICENSE`.
- Registrazioni: **Sam Gossner / Versilian Studios** e **Simon Dalzell / Ivy Audio**.
- Sample cutting originale: **Elan Hickler / Soundemote**.

Si conservano anche `Readme.txt` e `README.md` originali. La licenza delle
registrazioni non sostituisce quella del codice del plugin o di JUCE. Questo
banco è un adattamento per Sculpt Lead; non implica approvazione o produzione
del plugin da parte di Versilian Studios o degli autori delle registrazioni.
