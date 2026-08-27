# Multi-Branch TAGE Tasarimi

Bu not, gem5 icindeki `MyTAGE` wrapper'i ile
`src/cpu/pred/myTage_multi_entry` backend'inin mevcut davranisini
akademik pseudo-code biciminde ozetler. Temel fikir, bir fetch line icin TAGE
tablosunu aligned PC ile okumak ve ayni satirdaki birden fazla entry arasindan
gercek branch PC offset'i ile eslesen entry'yi secmektir.

## Guncel FS Parametreleri

Asagidaki degerler
`work/run_riscv_ubuntu_o3_mytage_from_checkpoint.sh` icindeki varsayilan FS/O3
test parametreleridir. Scriptte environment variable, CLI option,
`--tage-param` veya raw `--param` ile override edilebilir.

### CPU ve Fetch

| Parametre | Varsayilan | Kullanim |
| --- | ---: | --- |
| `FETCH_WIDTH` | 4 | O3 fetch genisligi. Bir cycle icinde decode'a gidebilecek maksimum instruction sayisini belirler. |
| `DECODE_WIDTH` | 4 | Decode genisligi. |
| `ISSUE_WIDTH` | 4 | Issue genisligi. Predictor algoritmasinin dogrudan parcasi degildir. |
| `COMMIT_WIDTH` | 4 | Commit genisligi. Predictor update'leri commit akisi ile gelir. |

### TAGE Genel Parametreleri

| Parametre | Varsayilan | Kullanim |
| --- | ---: | --- |
| `BIMODAL_DEPTH` | 1024 | Bimodal row sayisi. |
| `GHISTORY_LENGTH` | 250 | Global history register uzunlugu. |
| `PCHISTORY_LENGTH` | 32 | Path history register uzunlugu. |
| `COMP_COUNT` | 4 | Tagged TAGE component sayisi. |
| `ENTRY_PER_SET` | 2 | Her indexed TAGE row icindeki entry/way sayisi. |
| `FETCH_LINE_BYTES` | 16 | TAGE lookup icin aligned fetch-line byte sayisi. |
| `BIMODAL_CTRS_PER_ROW` | 8 | Bimodal row basina counter/bank sayisi. |
| `BIMODAL_OFFSET_SHIFT` | 1 | Bimodal bank seciminde byte offset shift'i. `16B >> 1 = 8` adet 2-byte slot verir. |
| `BIMODAL_USE_ALIGNED_ADDR` | True | Bimodal row index'i aligned PC'den uretilir. |
| `TAG_USE_ALIGNED_ADDR` | True | Tagged table tag hash'i aligned PC'den uretilir. |

### Hash ve Replacement Parametreleri

| Parametre | Varsayilan | Kullanim |
| --- | ---: | --- |
| `PC_HASH_START_FOR_IDX` | 2 | TAGE index PC hash baslangic biti. |
| `PC_HASH_WIDTH_FOR_IDX` | 10 | TAGE index PC hash genisligi. |
| `PC_HASH_START_FOR_TAG` | 10 | TAGE tag PC hash baslangic biti. |
| `PC_HASH_WIDTH_FOR_TAG` | 12 | TAGE tag PC hash genisligi. |
| `HISTORY_HASH_TYPE` | `CIRCULAR_SHIFT_REGISTER` | Global history hash metodu: CSR veya folded history. |
| `REPLACEMENT_MODE` | `USEFUL` | Allocation/replacement metodu: useful-bit tabanli veya PLRU. |
| `ALLOCATE_RANDOM_PLACEMENT` | True | Birden fazla allocation adayi varsa LFSR/SIMRAND ile secim yapabilir. |
| `ALLOCATE_LONGER_THAN_PROVIDER` | True | Provider varsa sadece daha uzun history'li tablolara allocate edilir. |
| `PERIODIC_RESET` | False | Useful bit periyodik reset mekanizmasi. |
| `PERIODIC_RESET_BRANCH_PERIOD` | 262144 | Periodic reset aktifse branch period'u. |
| `USE_LFSR` | True | Random secimler icin LFSR kullanilir. |
| `LFSR_WIDTH` | 32 | LFSR genisligi. |
| `LFSR_SEED` | `0xACE1` | LFSR seed. |
| `LFSR_MISPREDICTION_UPDATE` | True | Misprediction sonrasi LFSR state'i PC hash ile perturb edilir. |

### useAltOnNA Parametreleri

| Parametre | Varsayilan | Kullanim |
| --- | ---: | --- |
| `USE_ALT_ON_NA` | True | Weak provider icin alternate predictor secimini kontrol eder. |
| `NUM_USE_ALT_ON_NA` | 16 | useAltOnNA signed counter sayisi. |
| `USE_ALT_ON_NA_BITS` | 4 | useAltOnNA signed counter genisligi. |
| `USE_ALT_ON_NA_HASH_START` | 2 | useAltOnNA index PC hash baslangici. |
| `USE_ALT_ON_NA_HASH_WIDTH` | 4 | useAltOnNA index hash genisligi. |

### Table Vektorleri

| Parametre | Varsayilan |
| --- | --- |
| `TABLE_DEPTH` | `[256, 256, 256, 256]` |
| `TABLE_USEFUL_WIDTH` | `[2, 2, 2, 2]` |
| `TABLE_CTR_WIDTH` | `[3, 3, 3, 3]` |
| `TABLE_TAG_WIDTH` | `[8, 8, 9, 9]` |
| `TABLE_HISTORY_WIDTH` | `[5, 18, 68, 250]` |
| `TABLE_PC_HISTORY_START` | `[0, 0, 0, 0]` |
| `TABLE_PC_HISTORY_WIDTH` | `[3, 5, 16, 16]` |

## Veri Yapilari

Her tagged TAGE component `Ti` su fiziksel mantikla modellenir:

```text
Ti[tableDepth[i]][entryPerSet] -> Entry
Entry = {
    valid  : 1 bit,
    tag    : tableTagWidth[i] bit,
    offset : fetch-line offset,
    ctr    : tableCtrWidth[i] bit saturating counter,
    u      : tableUsefulWidth[i] bit useful counter
}
```

Mevcut C++ kodunda `offset = pc % fetchLineBytes` olarak tutulur. Bu nedenle
16-byte line icin byte-offset encoding kullanilirsa 4 bit gerekir. Fiziksel
tasarimda RISC-V compressed instruction alignment'i temel alinip
`slotOffset = (pc % fetchLineBytes) >> 1` tutulursa 16-byte line icin 8 slot
vardir ve 3 bit yeterlidir:

```text
byte offsets : 0, 2, 4, 6, 8, 10, 12, 14
slot offsets : 0, 1, 2, 3, 4, 5, 6, 7
```

Bimodal tablo banked row olarak modellenir:

```text
B[bimodalDepth][bimodalCtrsPerRow] -> 2-bit counter
```

Varsayilan 16-byte line ve `bimodalOffsetShift=1` icin bank secimi 2-byte
slot'a denk gelir.

## Alan Hesabi

16-byte fetch line ve 2-byte compressed alignment varsayimi ile slot-offset
encoding kullanilirse:

```text
bimodalCost =
    bimodalDepth * bimodalCtrWidth * bimodalCtrsPerRow

tageComponentCost[i] =
    tableDepth[i] * entryPerSet *
    (1 + tableTagWidth[i] + offsetWidth + tableCtrWidth[i] +
     tableUsefulWidth[i])

tageCost = sum_i(tageComponentCost[i])
totalCost = bimodalCost + tageCost + useAltOnNACost + historyStateCost

offsetWidth = log2(fetchLineBytes / minInstAlign)
            = log2(16 / 2)
            = 3

useAltOnNACost = numUseAltOnNa * useAltOnNaBits
```

Guncel FS varsayilanlariyla kaba storage hesabi:

```text
bimodalCost = 1024 * 2 * 8 = 16384 bit

T0 = 256 * 2 * (1 + 8 + 3 + 3 + 2) =  8704 bit
T1 = 256 * 2 * (1 + 8 + 3 + 3 + 2) =  8704 bit
T2 = 256 * 2 * (1 + 9 + 3 + 3 + 2) =  9216 bit
T3 = 256 * 2 * (1 + 9 + 3 + 3 + 2) =  9216 bit

tageCost = 35840 bit
useAltOnNACost = 16 * 4 = 64 bit
total ~= 52288 bit + history/LFSR/PLRU metadata
```

Eger byte-offset encoding kullanilirsa `offsetWidth=4` olur ve tagged table
cost'u her entry icin 1 bit artar.

## Algoritmalar

### Yardimci Fonksiyonlar

```text
AlignLine(pc, fetchLineBytes):
    return pc & ~(fetchLineBytes - 1)

ByteOffset(pc, fetchLineBytes):
    return pc mod fetchLineBytes

SlotOffset(pc, fetchLineBytes, minInstAlign):
    return ByteOffset(pc, fetchLineBytes) / minInstAlign

Offset(pc):
    if implementation stores byte offsets:
        return ByteOffset(pc, fetchLineBytes)
    if physical design stores 2-byte slots:
        return SlotOffset(pc, fetchLineBytes, minInstAlign = 2)

SatPred(ctr, ctrWidth):
    return ctr >= 2^(ctrWidth - 1)

WeakCounter(ctr, ctrWidth):
    midHi = 2^(ctrWidth - 1)
    midLo = midHi - 1
    return ctr == midHi or ctr == midLo

UseAltOnNAIndex(pc):
    hashWidth =
        useAltOnNaHashWidth > 0
            ? useAltOnNaHashWidth
            : log2(numUseAltOnNa)
    return GetIdx(pc, useAltOnNaHashStart, hashWidth) mod numUseAltOnNa

UseAltOnNAPrefersAlt(idx):
    return useAltOnNA[idx] >= 0
```

### Fetch-Line History Snapshot

Gem5 wrapper her conditional branch icin ayri `PredictorHistory` uretir.
Iki farkli snapshot tutulur:

```text
historyState:
    Bu branch'in speculative history repair snapshot'idir.
    Squash durumunda predictor bu snapshot'a geri doner.

lookupState:
    Bu branch icin lookup sirasinda hesaplanan provider, altpred,
    bimodal counter, idx, tag ve selected entry pointer state'idir.
    Commit update bu state uzerinden yapilir.
```

Ayni fetch line icindeki branch lookup'lari ayni aligned-line history base'i
ile calisir:

```text
StartLineContext(tid, alignedPC):
    ctx = lineContext[tid]
    if ctx.valid and ctx.alignedPC == alignedPC:
        return ctx.lookupBaseState

    ctx.lookupBaseState = SnapshotBackendState()
    ctx.valid = true
    ctx.alignedPC = alignedPC
    return ctx.lookupBaseState
```

Bu sayede line icindeki her branch:

```text
idx/tag hash input history = ctx.lookupBaseState
index PC                  = alignedPC
lookup PC                 = actual branch PC
```

ile sorgulanir. Branch outcome speculative history'e yine branch bazinda
eklenir; taken veya squash durumunda line context invalidate edilir.

### Fetch Bundle Akisi

Gem5 O3 fetch stage icinde en fazla `fetchWidth` adet instruction decode'a
hazirlanir. Branch predictor yine branch instruction basina cagrilir; fakat
ayni aligned fetch line icindeki branchler ayni `lookupBaseState` ile TAGE
index/tag hash'i uretir.

```text
FetchBundle(fetchPC, fetchWidth):
    alignedPC = AlignLine(fetchPC, fetchLineBytes)
    baseState = StartLineContext(tid, alignedPC)

    numInst = 0
    predictedBranch = false

    while numInst < fetchWidth and not predictedBranch:
        inst = DecodeNextInstFromFetchedBytes()

        if inst is not control-flow:
            numInst++
            continue

        pred, hist = Predict(tid, inst.pc)

        if pred == taken:
            predictedBranch = true
            nextPC = PredictedTarget(inst)
        else:
            nextPC = inst.fallthrough

        numInst++

    return nextPC
```

Bu modelde `fetchWidth` TAGE table row sayisini degistirmez; line icinde kac
branch icin ayni aligned lookup state'inin kullanilabilecegini belirleyen
frontend ust siniridir. Multi-entry TAGE'nin fiziksel amaci, tek aligned row
okumasindan bu line icindeki birden fazla branch offset'i icin cevap
uretebilmektir.

### Prediction

```text
Predict(tid, pc):
    alignedPC = AlignLine(pc, fetchLineBytes)
    baseState = StartLineContext(tid, alignedPC)

    hist = new PredictorHistory
    hist.historyState = SnapshotBackendState()

    pred, lookupState = TAGEPredict(
        indexAddr  = alignedPC,
        lookupAddr = pc,
        baseState  = baseState
    )

    hist.lookupState = lookupState
    hist.predTaken = pred
    return pred, hist
```

`TAGEPredict` algoritmasi:

```text
TAGEPredict(indexAddr, lookupAddr, baseState):
    state = copy(baseState)
    state.indexAddr = indexAddr
    state.lookupAddr = lookupAddr

    bimodalIndexAddr =
        bimodalUseAlignedAddr ? indexAddr : lookupAddr

    bimodalRow =
        GetIdx(bimodalIndexAddr, 0, log2(bimodalDepth))
    bimodalBank =
        ((lookupAddr mod fetchLineBytes) >> bimodalOffsetShift)
        mod bimodalCtrsPerRow

    state.bimodalCtr = B[bimodalRow][bimodalBank]
    state.bimodalPred = SatPred(state.bimodalCtr, 2)

    tagAddr = tagUseAlignedAddr ? indexAddr : lookupAddr

    matches = empty list
    offsetExists = false

    for i in 0 .. compCount - 1:
        idxWidth = log2(tableDepth[i])

        pcIdxHash =
            GetIdx(indexAddr,
                   pcHashStartForIdx + compCount - i,
                   pcHashWidthForIdx)

        if historyHashType == CIRCULAR_SHIFT_REGISTER:
            ghIdxHash = CSRIndexHashSnapshot[i] truncated to idxWidth
        else:
            ghIdxHash = FoldedGlobalHistory(
                ghistoryLength,
                tableHistoryWidth[i],
                idxWidth
            )

        if pchistoryLength != 0 and tablePcHistoryWidth[i] > 0:
            pathIdxHash = FoldedPathHistory(
                start = tablePcHistoryStart[i],
                width = tablePcHistoryWidth[i],
                outWidth = min(idxWidth, tablePcHistoryWidth[i])
            )
        else:
            pathIdxHash = 0

        idx[i] = (pcIdxHash xor ghIdxHash xor pathIdxHash)
                 & mask(idxWidth)

        pcTagHash =
            GetIdx(tagAddr, pcHashStartForTag, pcHashWidthForTag)

        if historyHashType == CIRCULAR_SHIFT_REGISTER:
            ghTagHash = CSRTagHashSnapshot[i] truncated to tableTagWidth[i]
        else:
            ghTagHash = FoldedGlobalHistory(
                ghistoryLength,
                tableHistoryWidth[i],
                tableTagWidth[i]
            )

        if pchistoryLength != 0 and tablePcHistoryWidth[i] > 0:
            pathTagHash = FoldedPathHistory(
                start = tablePcHistoryStart[i],
                width = tablePcHistoryWidth[i],
                outWidth = min(tableTagWidth[i], tablePcHistoryWidth[i])
            )
        else:
            pathTagHash = 0

        tag[i] = (pcTagHash xor ghTagHash xor pathTagHash)
                 & mask(tableTagWidth[i])

        for way in 0 .. entryPerSet - 1:
            e = T[i][idx[i]][way]

            if not e.valid:
                continue

            if e.offset != Offset(lookupAddr):
                continue

            offsetExists = true

            if e.tag == tag[i]:
                matches.push({
                    table = i,
                    way = way,
                    historyWidth = tableHistoryWidth[i],
                    entry = e
                })
                break

    if not offsetExists:
        state.predType = BIMODAL
        state.pred = state.bimodalPred
        return state.pred, state

    provider = LongestHistoryMatch(matches)
    altpred = SecondLongestHistoryMatch(matches, provider)

    if provider does not exist:
        state.predType = BIMODAL
        state.pred = state.bimodalPred
        return state.pred, state

    state.provider = provider
    state.providerPred =
        SatPred(provider.entry.ctr, tableCtrWidth[provider.table])
    state.providerPseudoNew =
        WeakCounter(provider.entry.ctr, tableCtrWidth[provider.table])

    if altpred exists:
        state.altpred = altpred
        state.altPredValue =
            SatPred(altpred.entry.ctr, tableCtrWidth[altpred.table])
    else:
        state.altPredValue = state.bimodalPred
        state.altpredIsBimodal = true

    chooseProvider = true

    if state.providerPseudoNew:
        if useAltOnNA:
            uidx = UseAltOnNAIndex(lookupAddr)
            state.useAltOnNAIndex = uidx
            state.useAltOnNAActive = true
            chooseProvider = not UseAltOnNAPrefersAlt(uidx)
        else:
            chooseProvider = false

    if chooseProvider:
        state.predType = PROVIDER
        state.pred = state.providerPred
        return state.pred, state

    if altpred exists:
        state.predType = ALTPRED
        state.pred = state.altPredValue
        return state.pred, state

    state.predType = BIMODAL_ALT
    state.pred = state.bimodalPred
    return state.pred, state
```

Provider seciminde en uzun `tableHistoryWidth` kazanir. Esitlik durumunda daha
yuksek table index'i secilir. Alternate predictor, provider disindaki en uzun
history match'tir.

### Speculative History Update

Gem5 prediction sonrasi branch history'yi speculative olarak ilerletir:

```text
UpdateHistories(tid, pc, uncond, taken, hist):
    if hist does not exist:
        hist = new PredictorHistory
        hist.historyState = SnapshotBackendState()
        if branch is conditional:
            hist.lookupState = TAGEPredict(
                AlignLine(pc, fetchLineBytes),
                pc,
                StartLineContext(tid, AlignLine(pc, fetchLineBytes))
            ).state

    UpdateHistoryOnly(pc, taken, hist.historyState)

    if taken:
        InvalidateLineContext(tid)
```

History-only update:

```text
UpdateHistoryOnly(pc, outcome, state):
    if historyHashType == CIRCULAR_SHIFT_REGISTER:
        for i in 0 .. compCount - 1:
            hlen = min(tableHistoryWidth[i], ghistoryLength)
            oldBit = GHR[hlen - 1]
            CSRIndexHash[i].update(newBit = outcome, oldBit)
            CSRTagHash[i].update(newBit = outcome, oldBit)

    GHR.push(outcome)

    if pchistoryLength != 0:
        pathBit = ((pc >> 2) xor (pc >> 5) xor (pc >> 11)) & 1
        PCHistory.push(pathBit)
```

### Commit Update

Final table update sadece commit tarafinda, `squashed=false` ile gelir. Bu
nedenle backend `totalBranchCount` ve `missPredictionCount` committed
conditional branch stream'i uzerinden artar.

```text
CommitUpdate(pc, outcome, hist):
    if hist.isConditional:
        TAGECommitUpdate(pc, outcome, hist.lookupState)

    delete hist
```

`TAGECommitUpdate`:

```text
TAGECommitUpdate(pc, outcome, state):
    predictionWrong = (state.pred != outcome)
    needAlloc = predictionWrong

    if replacementMode == PLRU:
        if state.predType == PROVIDER and provider exists:
            TouchPLRU(state.provider.table,
                      state.idx[state.provider.table],
                      state.provider.way)
        else if state.predType == ALTPRED and altpred exists:
            TouchPLRU(state.altpred.table,
                      state.idx[state.altpred.table],
                      state.altpred.way)

    if useAltOnNA and provider exists and state.providerPseudoNew:
        if state.providerPred == outcome and needAlloc:
            needAlloc = false

        if state.providerPred != state.altPredValue:
            altCorrect = (state.altPredValue == outcome)
            UpdateSignedCounter(
                useAltOnNA[state.useAltOnNAIndex],
                increment = altCorrect,
                width = useAltOnNaBits
            )

    if provider exists and not predictionWrong:
        noAllocationBecauseOfHit++

    if needAlloc:
        candidates = empty list

        for i in 0 .. compCount - 1:
            if provider exists and i == state.provider.table:
                continue

            if provider exists and allocateLongerThanProvider:
                if tableHistoryWidth[i] <=
                   tableHistoryWidth[state.provider.table]:
                    continue

            candidates.push(i)

        if candidates is empty:
            noAllocationBecauseOfProviderHigh++

        else if replacementMode == PLRU:
            allocTable = SelectAllocationTable(
                candidates,
                allocateRandomPlacement,
                useLfsr,
                lfsrWidth,
                lfsrSeed
            )

            allocWay = PLRUVictimWay(allocTable, state.idx[allocTable])
            AllocateEntry(allocTable, allocWay, state, outcome)
            TouchPLRU(allocTable, state.idx[allocTable], allocWay)

        else if replacementMode == USEFUL:
            allocatable = empty list

            for i in candidates:
                if ExistsWayWithUsefulZero(T[i][state.idx[i]]):
                    allocatable.push(i)

            if allocatable is empty:
                for i in candidates:
                    DecayUsefulBits(T[i][state.idx[i]])
                noAllocationBecauseOfNoFree++

            else:
                allocTable = SelectAllocationTable(
                    allocatable,
                    allocateRandomPlacement,
                    useLfsr,
                    lfsrWidth,
                    lfsrSeed
                )

                allocWay = FirstUsefulZeroWay(
                    T[allocTable][state.idx[allocTable]]
                )
                AllocateEntry(allocTable, allocWay, state, outcome)

    if replacementMode == USEFUL and provider exists:
        if state.providerPred != state.altPredValue:
            if state.providerPred == outcome:
                SaturatingIncrement(provider.u,
                                     tableUsefulWidth[provider.table])
            else:
                SaturatingDecrement(provider.u,
                                     tableUsefulWidth[provider.table])

    if provider exists:
        SaturatingUpdate(provider.ctr,
                         outcome,
                         tableCtrWidth[provider.table])
    else:
        SaturatingUpdate(state.bimodalCtr, outcome, 2)
        B[state.bimodalRow][state.bimodalBank] = state.bimodalCtr

    if replacementMode == USEFUL and periodicReset:
        usefulResetCounter++
        if usefulResetCounter == periodicResetBranchPeriod:
            ResetOneUsefulBitAcrossAllEntries()
            usefulResetCounter = 0

    if useLfsr and lfsrMispredictionUpdate and predictionWrong:
        updateAddr = state.lookupAddr != 0 ? state.lookupAddr : pc
        lfsr.update(updateAddr xor (updateAddr >> 3) xor (updateAddr << 5))

    RecordPredictionUse(state)
    totalBranchCount++
    if outcome:
        takenBranchCount++
    if predictionWrong:
        missPredictionCount++
    RecordPredictionHits(state, outcome)
```

Allocation:

```text
AllocateEntry(table, way, state, outcome):
    midHi = 2^(tableCtrWidth[table] - 1)
    midLo = midHi - 1

    e = T[table][state.idx[table]][way]
    e.valid = true
    e.tag = state.tag[table]
    e.offset = Offset(state.lookupAddr)
    e.u = 0
    e.ctr = outcome ? midHi : midLo
```

`SelectAllocationTable` algoritmasi:

```text
SelectAllocationTable(candidates, allocateRandomPlacement, useLfsr, ...):
    if allocateRandomPlacement and candidates.size > 1:
        if useLfsr:
            lfsr.next()
            return candidates[lfsr.fold(log2(candidates.size)) mod
                              candidates.size]
        else:
            return candidates[rand() mod candidates.size]

    return candidate with largest tableHistoryWidth
           tie-break: largest table index
```

### Squash ve Mispredict Repair

Gem5 yanlis tahmini execute/decode tarafinda fark ettiginde predictor'a
`squashed=true` update yollar. Bu yol table counter'larini egitmez; sadece
history state'i onarir.

```text
MispredictRepair(tid, pc, actualOutcome, hist):
    ResetBackendState(hist.historyState)
    UpdateHistoryOnly(pc, actualOutcome, hist.historyState)
    InvalidateLineContext(tid)
```

Daha genc, tamamen squash edilen branchler icin:

```text
SquashYoungerBranch(tid, hist):
    ResetBackendState(hist.historyState)
    InvalidateLineContext(tid)
    delete hist
```

Bu ayrim onemlidir: mispredict repair global/path history'yi dogru branch
outcome'u ile tamir eder, fakat TAGE table allocation/counter update commit
gelene kadar yapilmaz.

## Counter Mantigi

Backend istatistikleri commit update sirasinda artar:

```text
totalBranches      = committed conditional branch count
missPredictions    = count(outcome != savedPrediction)
correctPredictions = totalBranches - missPredictions
accuracy           = correctPredictions / totalBranches
```

Predictor lookup sayaci `predictionLookups`, squash/yanlis-path branchleri de
icerebilir. Buna karsin `totalBranches` ve `missPredictions` committed branch
stream'i uzerindendir.
