# hotLoop Control Flow Graph (diagram)

Method: `OSRCrashTest.hotLoop (I)I`  |  OSR @ bci 4

Registers in loop
- x0/w0: i
- x1/w1: seed (argument)
- x2/w2: sum

Diagram

```
          ┌────────────────────────────────────────────────┐
          │ B7 (Entry)                                     │
          │  0x108546c40: prologue, MDO counters           │
          └───────┬────────────────────────────────────────┘
                  │
                  ▼
          ┌────────────────────────────────────────────────┐
          │ B8                                             │
          └───────┬────────────────────────────────────────┘
                  │
                  ▼
          ┌────────────────────────────────────────────────┐
          │ B0 (Init)                                      │
          │  0x108546c7c: w0=0 (i), w2=0 (sum)             │
          │  goto B1                                       │
          └───────┬────────────────────────────────────────┘
                  │
                  ▼
          ┌────────────────────────────────────────────────┐
          │ B1 (Loop Header)                               │
          │  0x108546d2c: cmp w0, #50000                   │
          │  b.lt → B2    else → B3                        │
          └───────┬───────────────┬────────────────────────┘
                  │               │
                  │               ▼
                  │       ┌────────────────────────────────┐
                  │       │ B3 (Exit)                      │
                  │       │  0x108546d5c: x0=x2; epilogue │
                  │       │  ret                           │
                  │       └────────────────────────────────┘
                  │
                  ▼
        ┌──────────────────────────────────────────────────┐
        │ B2 (Body)                                        │
        │  0x108546c88: w3=w1*w0; w2=w3+w2                 │
        │  0x108546c98: cmp w2, #1_000_000                 │
        │  b.le → B5    else → B4                          │
        └───────────┬───────────────────────┬──────────────┘
                    │                       │
                    ▼                       ▼
     ┌─────────────────────────┐   ┌────────────────────────┐
     │ B5 (Inc/Backedge)       │   │ B4 (Modulo Branch)     │
     │ 0x108546cd8: w0 = w0+1  │   │ 0x108546cc4: w2 %=1000 │
     │ counters/poll           │   │ goto B5                │
     │ goto B1                 │   └───────────┬────────────┘
     └───────────┬────────────┘               │
                 └─────────────────────────────┘

OSR entry (bci:4)
┌───────────────────────────────────────────────────────────┐
│ B6 (OSR Entry)                                            │
│ 0x108546d74:                                              │
│   build_frame(...)                                        │
│   w2=[x1+16] (seed), w3=[x1+8] (sum), w4=[x1+0] (i)       │
│   spill → call OSR_migration_end → restore                │
│   x0=i, x2=sum, x1=seed → goto B1                         │
└───────────────────────────────────────────────────────────┘
```

Key addresses
- B7: 0x0000000108546c40 (entry)
- B0: 0x0000000108546c7c (init)
- B1: 0x0000000108546d2c (header)
- B2: 0x0000000108546c88 (sum += i*seed)
- B4: 0x0000000108546cc4 (modulo)
- B5: 0x0000000108546cd8 (i++, backedge)
- B3: 0x0000000108546d5c (return)
- B6: 0x0000000108546d74 (OSR entry with prologue)

Notes
- Loop now mirrors the normal entry/exit even on OSR path thanks to `build_frame`.
- Keep this CFG as part of regression docs for future C1 OSR changes.
